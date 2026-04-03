// ============================================================================
// LLVMIRToVDRMTPass.cpp — LLVM IR → vDRMT structural pattern matching
//
// Three recognised block patterns:
//
//   Pattern A – pure comparison (yields control-flow successor):
//     GEP/load chain from arg0 → icmp const → cond_br bb1/bb2
//     where bb1: ret i32 N, bb2: ret i32 M
//     → vdrmt.read + struct_extract chain + vdrmt.cmp
//       + vdrmt.if{vdrmt.next N} else{vdrmt.next M}
//
//   Pattern B – field read → compute → write to metadata:
//     gep arg1 field F → load from arg0 headers → arith → store to meta
//     → vdrmt.struct_extract_ref + vdrmt.read + struct_extract + arith
//       + vdrmt.assign + vdrmt.next -1
//
//   Pattern C – constant store to metadata:
//     gep arg1 field F → store const → ret -1
//     → vdrmt.struct_extract_ref + vdrmt.constant + vdrmt.assign + vdrmt.next -1
//
// Type mapping:
//   %T = type { f0, f1, …, i1 }  → !vdrmt.header<"T", field_0:f0,…,__valid:i1>
//   %T = type { f0, f1, … }       → !vdrmt.struct<"T", field_0:f0, …>
//   ptr (unresolved)               → !vdrmt.ref<i8>
// ============================================================================

#include "Pass/LLVMIRToVDRMTPass.h"

#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Target/LLVMIR/Import.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMIRToLLVMTranslation.h"
#include "mlir/InitAllDialects.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"

#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace mlir;

// ============================================================================
// Type conversion: LLVM struct types → vDRMT struct / header types
// ============================================================================

// Forward declaration for recursion.
static Type llvmTypeToVDRMT(Type llvmTy, MLIRContext *ctx);

static Type llvmStructToVDRMT(LLVM::LLVMStructType structTy, MLIRContext *ctx) {
    StringRef name = structTy.isIdentified() ? structTy.getName() : "anon";
    ArrayRef<Type> body = structTy.getBody();

    // Heuristic: last field is i1 → validity bit → header type.
    bool isHeader = !body.empty() && body.back().isInteger(1);

    SmallVector<vdrmt::FieldInfo> fields;
    for (unsigned i = 0, n = body.size(); i < n; ++i) {
        StringAttr fname;
        if (isHeader && i == n - 1)
            fname = StringAttr::get(ctx, "__valid");
        else
            fname = StringAttr::get(ctx, "field_" + std::to_string(i));

        Type fty = llvmTypeToVDRMT(body[i], ctx);
        fields.push_back(vdrmt::FieldInfo(fname, fty, {}));
    }

    if (isHeader)
        return vdrmt::HeaderType::get(ctx, name, fields);
    return vdrmt::StructType::get(ctx, name, fields);
}

static Type llvmTypeToVDRMT(Type llvmTy, MLIRContext *ctx) {
    if (isa<IntegerType>(llvmTy))
        return llvmTy;
    if (auto st = dyn_cast<LLVM::LLVMStructType>(llvmTy))
        return llvmStructToVDRMT(st, ctx);
    if (isa<LLVM::LLVMPointerType>(llvmTy))
        return vdrmt::ReferenceType::get(ctx, IntegerType::get(ctx, 8));
    return llvmTy; // fallback (should not happen for well-formed IR)
}

// ============================================================================
// Helpers on vDRMT struct/header types
// ============================================================================

static Type getFieldType(Type structOrHeaderTy, int32_t idx) {
    if (auto st = dyn_cast<vdrmt::StructType>(structOrHeaderTy))
        return st.getElements()[idx].type;
    if (auto ht = dyn_cast<vdrmt::HeaderType>(structOrHeaderTy))
        return ht.getElements()[idx].type;
    return {};
}

static StringRef getFieldName(Type structOrHeaderTy, int32_t idx) {
    if (auto st = dyn_cast<vdrmt::StructType>(structOrHeaderTy))
        return st.getElements()[idx].name.getValue();
    if (auto ht = dyn_cast<vdrmt::HeaderType>(structOrHeaderTy))
        return ht.getElements()[idx].name.getValue();
    return "field_?";
}

// ============================================================================
// LLVM icmp predicate → vDRMT CmpOpKind
// ============================================================================

static std::optional<int32_t> icmpToVDRMTKind(LLVM::ICmpPredicate pred) {
    using P = LLVM::ICmpPredicate;
    switch (pred) {
    case P::eq:  return 0; // Eq
    case P::ne:  return 1; // Ne
    case P::slt: case P::ult: return 2; // Lt
    case P::sle: case P::ule: return 3; // Le
    case P::sgt: case P::ugt: return 4; // Gt
    case P::sge: case P::uge: return 5; // Ge
    default: return std::nullopt;
    }
}

// ============================================================================
// LLVM binary op → vDRMT BinOpKind
// ============================================================================

static std::optional<vdrmt::BinOpKind> llvmBinOpToVDRMT(Operation &op) {
    if (isa<LLVM::AndOp>(op)) return vdrmt::BinOpKind::And;
    if (isa<LLVM::OrOp>(op))  return vdrmt::BinOpKind::Or;
    if (isa<LLVM::XOrOp>(op)) return vdrmt::BinOpKind::Xor;
    if (isa<LLVM::AddOp>(op)) return vdrmt::BinOpKind::Add;
    if (isa<LLVM::SubOp>(op)) return vdrmt::BinOpKind::Sub;
    if (isa<LLVM::MulOp>(op)) return vdrmt::BinOpKind::Mul;
    return std::nullopt;
}

// ============================================================================
// Per-function converter
// ============================================================================

/// Pointer provenance: tracks which function argument a pointer derives from
/// and the GEP field-index path from that argument.
struct PtrProv {
    int argIdx = -1;                   // index into func args; -1 = unknown
    std::vector<int32_t> path;         // field indices from the arg root

    bool valid() const { return argIdx >= 0; }
};

/// Byte-swap helpers for constant-folding htonl/htons/ntohl/ntohs.
static uint32_t bswap32(uint32_t v) {
    return ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) |
           ((v & 0x0000ff00u) << 8)  | ((v & 0x000000ffu) << 24);
}
static uint16_t bswap16(uint16_t v) {
    return (uint16_t)(((v & 0xff00u) >> 8) | ((v & 0x00ffu) << 8));
}

/// Get the integer constant from an LLVM constant op result, if possible.
static std::optional<int64_t> getConstInt(Value v) {
    if (!v) return std::nullopt;
    if (auto cst = v.getDefiningOp<LLVM::ConstantOp>()) {
        if (auto iattr = dyn_cast<IntegerAttr>(cst.getValue()))
            return iattr.getInt();
    }
    return std::nullopt;
}

/// Get the successor block ID from a block that is expected to contain
/// only a single `ret i32 N` instruction.
static std::optional<int32_t> getRetSuccessor(Block *block) {
    if (!block || block->getNumArguments() != 0) return std::nullopt;
    if (block->getOperations().size() == 1) {
        auto &op = block->front();
        if (auto ret = dyn_cast<LLVM::ReturnOp>(op)) {
            if (ret.getNumOperands() == 1) {
                if (auto v = getConstInt(ret.getOperand(0)))
                    return (int32_t)*v;
            }
        }
    }
    return std::nullopt;
}

class FunctionConverter {
public:
    LLVM::LLVMFuncOp llvmFunc;
    MLIRContext *ctx;
    OpBuilder &modBuilder;
    int blockId;

    // Alias map: alloca ptr → ptr it was copy-initialized from.
    DenseMap<Value, Value> aliasMap;

    // Memoised provenance for pointer Values.
    DenseMap<Value, PtrProv> provCache;

    // Inferred LLVM struct type for each function arg (by arg index).
    SmallVector<LLVM::LLVMStructType> argLLVMStructTypes; // may be null
    // Corresponding vDRMT type (after conversion).
    SmallVector<Type> argVDRMTTypes;

    // vDRMT function argument Values (after creating the func.func).
    SmallVector<Value> vdrmtArgs;

    // LLVM SSA Value → vDRMT SSA Value.
    DenseMap<Value, Value> valMap;

    // Cache for vdrmt.read results: argIdx → read result.
    DenseMap<int, Value> readCache;
    // Cache for vdrmt.struct_extract chains: (argIdx, path) → extracted Value.
    std::map<std::pair<int, std::vector<int32_t>>, Value> extractCache;

    FunctionConverter(LLVM::LLVMFuncOp f, MLIRContext *c, OpBuilder &mb, int id)
        : llvmFunc(f), ctx(c), modBuilder(mb), blockId(id) {}

    // ------------------------------------------------------------------
    // Phase 1: build alias map (alloca → source ptr)
    // ------------------------------------------------------------------
    void buildAliasMap() {
        llvmFunc.walk([&](LLVM::StoreOp store) {
            Value dest = store.getAddr();
            Value src  = store.getValue();
            if (!dest.getDefiningOp<LLVM::AllocaOp>()) return;
            if (auto loadOp = src.getDefiningOp<LLVM::LoadOp>())
                aliasMap[dest] = loadOp.getAddr();
        });
    }

    // Resolve alias chain: follow aliasMap until stable.
    Value resolveAlias(Value v) {
        while (aliasMap.count(v)) v = aliasMap[v];
        return v;
    }

    // ------------------------------------------------------------------
    // Phase 2: pointer provenance
    // ------------------------------------------------------------------
    PtrProv getProvenance(Value ptr) {
        Value resolved = resolveAlias(ptr);

        auto it = provCache.find(resolved);
        if (it != provCache.end()) return it->second;

        PtrProv prov;

        // Is it a function argument?
        if (auto ba = dyn_cast<BlockArgument>(resolved)) {
            Block *entry = &llvmFunc.getBody().front();
            if (ba.getOwner() == entry) {
                prov.argIdx = (int)ba.getArgNumber();
                prov.path   = {};
            }
        }
        // Is it a GEP?
        else if (auto gep = resolved.getDefiningOp<LLVM::GEPOp>()) {
            PtrProv base = getProvenance(gep.getBase());
            if (base.valid()) {
                prov = base;
                // rawConstantIndices[0] is array offset (always 0 for structs),
                // subsequent indices are struct field indices.
                auto indices = gep.getRawConstantIndices();
                for (size_t i = 1; i < indices.size(); ++i) {
                    int32_t raw = indices[i];
                    if (raw == std::numeric_limits<int32_t>::min()) {
                        // Dynamic index: cannot statically determine → unknown
                        prov = {};
                        break;
                    }
                    prov.path.push_back(raw);
                }
            }
        }

        provCache[resolved] = prov;
        return prov;
    }

    // ------------------------------------------------------------------
    // Phase 3: infer arg struct types by scanning GEPs and loads
    // ------------------------------------------------------------------
    void inferArgStructTypes() {
        unsigned numArgs = llvmFunc.getNumArguments();
        argLLVMStructTypes.resize(numArgs);
        argVDRMTTypes.resize(numArgs);

        auto trySetType = [&](Value ptr, LLVM::LLVMStructType structTy) {
            PtrProv prov = getProvenance(ptr);
            if (prov.valid() && prov.path.empty() &&
                (unsigned)prov.argIdx < numArgs &&
                !argLLVMStructTypes[prov.argIdx]) {
                argLLVMStructTypes[prov.argIdx] = structTy;
                argVDRMTTypes[prov.argIdx] = llvmStructToVDRMT(structTy, ctx);
            }
        };

        llvmFunc.walk([&](LLVM::LoadOp load) {
            if (auto st = dyn_cast<LLVM::LLVMStructType>(load.getRes().getType()))
                trySetType(load.getAddr(), st);
        });
        llvmFunc.walk([&](LLVM::GEPOp gep) {
            if (auto st = dyn_cast<LLVM::LLVMStructType>(gep.getElemType()))
                trySetType(gep.getBase(), st);
        });
        llvmFunc.walk([&](LLVM::AllocaOp alloca) {
            if (auto st = dyn_cast<LLVM::LLVMStructType>(alloca.getElemType())) {
                // Find where this alloca was copy-initialized from.
                if (aliasMap.count(alloca.getRes())) {
                    Value src = resolveAlias(alloca.getRes());
                    PtrProv prov = getProvenance(src);
                    if (prov.valid() && prov.path.empty() &&
                        (unsigned)prov.argIdx < numArgs &&
                        !argLLVMStructTypes[prov.argIdx]) {
                        argLLVMStructTypes[prov.argIdx] = st;
                        argVDRMTTypes[prov.argIdx] = llvmStructToVDRMT(st, ctx);
                    }
                }
            }
        });

        // Fill in any unknown args with opaque i8 ref.
        for (unsigned i = 0; i < numArgs; ++i) {
            if (!argVDRMTTypes[i])
                argVDRMTTypes[i] = IntegerType::get(ctx, 8);
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: generate read/extract chain for (argIdx, path)
    // ------------------------------------------------------------------
    // Returns the vDRMT SSA value obtained by reading arg[argIdx] and
    // then extracting along the GEP path.
    Value generateReadChain(int argIdx, const std::vector<int32_t> &path,
                            Location loc, OpBuilder &builder) {
        // Ensure the root read exists.
        auto rootKey = std::make_pair(argIdx, std::vector<int32_t>{});
        if (!extractCache.count(rootKey)) {
            Type objTy = argVDRMTTypes[argIdx];
            Value readVal = builder.create<vdrmt::ReadOp>(loc, objTy, vdrmtArgs[argIdx]);
            extractCache[rootKey] = readVal;
        }

        Value curr = extractCache[rootKey];
        Type  currTy = argVDRMTTypes[argIdx];
        std::vector<int32_t> currPath;

        for (int32_t fi : path) {
            currPath.push_back(fi);
            auto key = std::make_pair(argIdx, currPath);
            if (!extractCache.count(key)) {
                Type fieldTy = getFieldType(currTy, fi);
                if (!fieldTy) {
                    llvm::errs() << "  ✗ LLVMIRToVDRMT: field type unknown at "
                                 << "arg" << argIdx << " path[" << fi << "]\n";
                    return {};
                }
                Value extracted = builder.create<vdrmt::StructExtractOp>(
                    loc, fieldTy, curr, builder.getI32IntegerAttr(fi));
                extractCache[key] = extracted;
                curr   = extracted;
                currTy = fieldTy;
            } else {
                curr   = extractCache[key];
                currTy = getFieldType(currTy, fi);
            }
        }
        return curr;
    }

    // ------------------------------------------------------------------
    // Phase 5: generate struct_extract_ref chain for writing to (argIdx, path)
    // ------------------------------------------------------------------
    Value generateWriteRef(int argIdx, const std::vector<int32_t> &path,
                           Location loc, OpBuilder &builder) {
        Value currRef = vdrmtArgs[argIdx];
        Type  currObjTy = argVDRMTTypes[argIdx];

        for (int32_t fi : path) {
            Type fieldTy = getFieldType(currObjTy, fi);
            if (!fieldTy) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: write-ref field unknown\n";
                return {};
            }
            Type refFieldTy = vdrmt::ReferenceType::get(ctx, fieldTy);
            currRef = builder.create<vdrmt::StructExtractRefOp>(
                loc, refFieldTy, currRef, builder.getI32IntegerAttr(fi));
            currObjTy = fieldTy;
        }
        return currRef;
    }

    // ------------------------------------------------------------------
    // Lazy value lookup: returns the vDRMT value for an LLVM value.
    // If the value is an integer constant not yet in valMap, emits it now.
    // ------------------------------------------------------------------
    Value getOrEmitValue(Value llvmVal, OpBuilder &builder) {
        if (Value v = valMap.lookup(llvmVal))
            return v;
        // Lazily emit integer constants on first use.
        if (auto cst = llvmVal.getDefiningOp<LLVM::ConstantOp>()) {
            if (auto iattr = dyn_cast<IntegerAttr>(cst.getValue())) {
                Value v = builder.create<vdrmt::ConstantOp>(
                    cst.getLoc(), iattr);
                valMap[llvmVal] = v;
                return v;
            }
        }
        // Constant-fold known byte-swap library functions (htonl/ntohl/htons/ntohs).
        if (auto callOp = llvmVal.getDefiningOp<LLVM::CallOp>()) {
            auto callee = callOp.getCallee();
            if (callee && callOp.getNumOperands() == 1) {
                Value argVal = getOrEmitValue(callOp.getOperand(0), builder);
                if (argVal) {
                    if (auto cstOp = argVal.getDefiningOp<vdrmt::ConstantOp>()) {
                        int64_t raw = cast<IntegerAttr>(cstOp.getValue()).getInt();
                        int64_t result = raw;
                        if (*callee == "htonl" || *callee == "ntohl")
                            result = (int64_t)(int32_t)bswap32((uint32_t)(int32_t)raw);
                        else if (*callee == "htons" || *callee == "ntohs")
                            result = (int64_t)(int16_t)bswap16((uint16_t)(int16_t)raw);
                        if (result != raw || *callee == "htonl" || *callee == "ntohl" ||
                            *callee == "htons" || *callee == "ntohs") {
                            Type ty = llvmVal.getType().isa<IntegerType>()
                                        ? llvmVal.getType()
                                        : builder.getI32Type();
                            auto attr = IntegerAttr::get(ty, result);
                            Value v = builder.create<vdrmt::ConstantOp>(callOp.getLoc(), attr);
                            valMap[llvmVal] = v;
                            return v;
                        }
                    }
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------
    // Convert a single op in the entry block.  Returns failure() for
    // unhandled ops that are not safely ignorable.
    // ------------------------------------------------------------------
    LogicalResult convertOp(Operation &op, OpBuilder &builder) {
        Location loc = op.getLoc();

        // ── Skip / alias-handled ops ────────────────────────────────
        if (isa<LLVM::AllocaOp>(op))
            return success(); // handled via aliasMap

        if (auto store = dyn_cast<LLVM::StoreOp>(op)) {
            Value dest = store.getAddr();
            // Alloca initialization stores were already folded into aliasMap;
            // skip them so we don't double-generate assigns.
            if (dest.getDefiningOp<LLVM::AllocaOp>())
                return success();

            // Real write to (possibly) metadata or headers.
            PtrProv destProv = getProvenance(dest);
            if (!destProv.valid()) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: store to unknown ptr\n";
                return failure();
            }
            Value srcVal = getOrEmitValue(store.getValue(), builder);
            if (!srcVal) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: store src not in valMap\n";
                return failure();
            }
            Value destRef = generateWriteRef(
                destProv.argIdx, destProv.path, loc, builder);
            if (!destRef) return failure();
            builder.create<vdrmt::AssignOp>(loc, srcVal, destRef);
            return success();
        }

        if (auto gep = dyn_cast<LLVM::GEPOp>(op)) {
            (void)gep; // provenance-only; no vDRMT op needed
            return success();
        }

        // ── Loads ─────────────────────────────────────────────────
        if (auto load = dyn_cast<LLVM::LoadOp>(op)) {
            PtrProv prov = getProvenance(load.getAddr());
            if (!prov.valid()) {
                // Could be a load from a local (e.g., metadata field loaded
                // for computation).  Skip silently for now.
                return success();
            }
            Value v = generateReadChain(prov.argIdx, prov.path, loc, builder);
            if (!v) return failure();
            valMap[load.getResult()] = v;
            return success();
        }

        // ── Constants ─────────────────────────────────────────────
        // Constants are emitted lazily by getOrEmitValue() at use sites.
        if (isa<LLVM::ConstantOp>(op))
            return success();

        // ── Binary ops ─────────────────────────────────────────────
        if (auto kind = llvmBinOpToVDRMT(op)) {
            Value lhs = getOrEmitValue(op.getOperand(0), builder);
            Value rhs = getOrEmitValue(op.getOperand(1), builder);
            if (!lhs || !rhs) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: binop operand not in valMap\n";
                return failure();
            }
            Value v = builder.create<vdrmt::BinOp>(
                loc, lhs.getType(), *kind, lhs, rhs);
            valMap[op.getResult(0)] = v;
            return success();
        }

        // ── Shifts ─────────────────────────────────────────────────
        if (auto shl = dyn_cast<LLVM::ShlOp>(op)) {
            Value lhs = getOrEmitValue(shl.getLhs(), builder);
            Value rhs = getOrEmitValue(shl.getRhs(), builder);
            if (!lhs || !rhs) return failure();
            Value v = builder.create<vdrmt::ShlOp>(loc, lhs, rhs);
            valMap[shl.getResult()] = v;
            return success();
        }
        if (auto shr = dyn_cast<LLVM::LShrOp>(op)) {
            Value lhs = getOrEmitValue(shr.getLhs(), builder);
            Value rhs = getOrEmitValue(shr.getRhs(), builder);
            if (!lhs || !rhs) return failure();
            Value v = builder.create<vdrmt::ShrOp>(loc, lhs, rhs);
            valMap[shr.getResult()] = v;
            return success();
        }

        // ── Casts ──────────────────────────────────────────────────
        if (isa<LLVM::SExtOp, LLVM::TruncOp,
                LLVM::PtrToIntOp, LLVM::IntToPtrOp, LLVM::BitcastOp>(op)) {
            Value src = getOrEmitValue(op.getOperand(0), builder);
            if (!src) return success(); // skip if src unknown
            Type dstTy = llvmTypeToVDRMT(op.getResult(0).getType(), ctx);
            Value v = builder.create<vdrmt::CastOp>(loc, dstTy, src);
            valMap[op.getResult(0)] = v;
            return success();
        }

        // ── Comparison ─────────────────────────────────────────────
        if (auto icmp = dyn_cast<LLVM::ICmpOp>(op)) {
            auto kind = icmpToVDRMTKind(icmp.getPredicate());
            if (!kind) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: unhandled icmp predicate\n";
                return failure();
            }
            Value lhs = getOrEmitValue(icmp.getLhs(), builder);
            Value rhs = getOrEmitValue(icmp.getRhs(), builder);
            if (!lhs || !rhs) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: icmp operands not in valMap\n";
                return failure();
            }
            Value v = builder.create<vdrmt::CmpOp>(
                loc, lhs, rhs, builder.getI32IntegerAttr(*kind));
            valMap[icmp.getResult()] = v;
            return success();
        }

        // ── Function calls ─────────────────────────────────────────
        // Known byte-swap calls are folded lazily by getOrEmitValue().
        // Unknown calls: skip silently (the result won't be in valMap).
        if (isa<LLVM::CallOp>(op))
            return success();

        // ── ZExt ───────────────────────────────────────────────────
        if (auto zext = dyn_cast<LLVM::ZExtOp>(op)) {
            Value src = getOrEmitValue(zext.getArg(), builder);
            if (src) {
                Type dstTy = llvmTypeToVDRMT(zext.getResult().getType(), ctx);
                Value v = builder.create<vdrmt::CastOp>(loc, dstTy, src);
                valMap[zext.getResult()] = v;
            }
            return success();
        }

        // ── Terminators ────────────────────────────────────────────
        // cond_br handled separately in convertEntryBlock.
        // ret handled separately.
        if (op.hasTrait<OpTrait::IsTerminator>())
            return success(); // handled by caller

        // ── Unknown non-terminator: warn but don't fail ────────────
        if (op.getNumResults() == 0)
            return success();
        llvm::errs() << "  ⚠ LLVMIRToVDRMT: unrecognised op " << op.getName()
                     << " — result(s) will be missing from valMap\n";
        return success();
    }

    // ------------------------------------------------------------------
    // Convert the entire function and append to modBuilder.
    // ------------------------------------------------------------------
    LogicalResult convert() {
        // Phase 1–3: analysis
        buildAliasMap();
        inferArgStructTypes();

        unsigned numArgs = llvmFunc.getNumArguments();
        auto loc = llvmFunc.getLoc();

        // Build function argument types: !vdrmt.ref<argVDRMTType>
        SmallVector<Type> funcArgTypes;
        for (unsigned i = 0; i < numArgs; ++i)
            funcArgTypes.push_back(
                vdrmt::ReferenceType::get(ctx, argVDRMTTypes[i]));

        // Build the output func.func.
        std::string funcName = "vdrmt_block" + std::to_string(blockId);
        auto voidTy = FunctionType::get(ctx, funcArgTypes, {});
        auto funcOp = modBuilder.create<func::FuncOp>(loc, funcName, voidTy);
        funcOp->setAttr("vdrmt.block_id", modBuilder.getI32IntegerAttr(blockId));

        // Populate function body.
        Block *entry = funcOp.addEntryBlock();
        OpBuilder builder(ctx);
        builder.setInsertionPointToEnd(entry);

        // Bind vDRMT arg values.
        for (unsigned i = 0; i < numArgs; ++i)
            vdrmtArgs.push_back(entry->getArgument(i));

        // Process the LLVM entry block.
        Region &llvmRegion = llvmFunc.getBody();
        if (llvmRegion.empty()) {
            // Declaration — remove the created function stub.
            funcOp.erase();
            return success();
        }

        Block &llvmEntry = llvmRegion.front();
        if (failed(convertEntryBlock(llvmEntry, builder)))
            return failure();

        return success();
    }

private:
    // Convert the entry block.  Handles the exit terminator specially.
    LogicalResult convertEntryBlock(Block &llvmEntry, OpBuilder &builder) {
        Location loc = llvmFunc.getLoc();

        // Convert all non-terminator ops.
        for (Operation &op : llvmEntry) {
            if (op.hasTrait<OpTrait::IsTerminator>()) break;
            if (failed(convertOp(op, builder))) return failure();
        }

        // Handle terminator.
        Operation *term = llvmEntry.getTerminator();
        if (!term) return success();

        if (auto ret = dyn_cast<LLVM::ReturnOp>(term)) {
            // Unconditional return.
            int32_t succ = -1;
            if (ret.getNumOperands() == 1)
                if (auto v = getConstInt(ret.getOperand(0)))
                    succ = (int32_t)*v;
            builder.create<vdrmt::NextOp>(loc, succ);
            builder.create<func::ReturnOp>(loc);
            return success();
        }

        if (auto condBr = dyn_cast<LLVM::CondBrOp>(term)) {
            // Conditional branch: expect successor blocks each containing
            // a single `ret i32 N`.
            auto trueSucc  = getRetSuccessor(condBr.getTrueDest());
            auto falseSucc = getRetSuccessor(condBr.getFalseDest());

            if (!trueSucc || !falseSucc) {
                // Successor blocks are more complex — recurse into them.
                return convertMultiBlockCFG(condBr, builder);
            }

            Value cond = valMap.lookup(condBr.getCondition());
            if (!cond) {
                llvm::errs() << "  ✗ LLVMIRToVDRMT: cond_br condition not in valMap\n";
                return failure();
            }

            int32_t ts = *trueSucc, fs = *falseSucc;
            builder.create<vdrmt::IfOp>(loc, cond, /*withElseRegion=*/true,
                [ts](OpBuilder &b, Location l) { b.create<vdrmt::NextOp>(l, ts); },
                [fs](OpBuilder &b, Location l) { b.create<vdrmt::NextOp>(l, fs); });
            builder.create<func::ReturnOp>(loc);
            return success();
        }

        if (auto br = dyn_cast<LLVM::BrOp>(term)) {
            // Unconditional branch — fall through to successor block.
            return convertSingleSuccessorBlock(br.getDest(), builder);
        }

        llvm::errs() << "  ✗ LLVMIRToVDRMT: unhandled entry-block terminator: "
                     << term->getName() << "\n";
        return failure();
    }

    // Handle entry → single successor (e.g. fall-through) pattern.
    LogicalResult convertSingleSuccessorBlock(Block *succ, OpBuilder &builder) {
        Location loc = llvmFunc.getLoc();
        // Process any ops in the successor before its terminator.
        for (Operation &op : *succ) {
            if (op.hasTrait<OpTrait::IsTerminator>()) break;
            if (failed(convertOp(op, builder))) return failure();
        }
        // Then handle the successor's own terminator.
        if (Operation *term = succ->getTerminator()) {
            if (auto ret = dyn_cast<LLVM::ReturnOp>(term)) {
                int32_t s = -1;
                if (ret.getNumOperands() == 1)
                    if (auto v = getConstInt(ret.getOperand(0)))
                        s = (int32_t)*v;
                builder.create<vdrmt::NextOp>(loc, s);
                builder.create<func::ReturnOp>(loc);
                return success();
            }
            if (auto condBr = dyn_cast<LLVM::CondBrOp>(term)) {
                // Re-enter the cond-br handling in the successor block.
                auto trueSucc  = getRetSuccessor(condBr.getTrueDest());
                auto falseSucc = getRetSuccessor(condBr.getFalseDest());
                if (!trueSucc || !falseSucc)
                    return convertMultiBlockCFG(condBr, builder);
                Value cond = valMap.lookup(condBr.getCondition());
                if (!cond) return failure();
                int32_t ts2 = *trueSucc, fs2 = *falseSucc;
                builder.create<vdrmt::IfOp>(loc, cond, /*withElseRegion=*/true,
                    [ts2](OpBuilder &b, Location l) { b.create<vdrmt::NextOp>(l, ts2); },
                    [fs2](OpBuilder &b, Location l) { b.create<vdrmt::NextOp>(l, fs2); });
                builder.create<func::ReturnOp>(loc);
                return success();
            }
        }
        builder.create<vdrmt::NextOp>(loc, -1);
        builder.create<func::ReturnOp>(loc);
        return success();
    }

    // Recursively convert a block and its successors into vDRMT ops,
    // suitable for use inside a vdrmt.if region (NoTerminator).
    // Unrolls join points by duplicating their content in each branch.
    // Does NOT emit func::ReturnOp (caller is responsible at function level).
    void convertBlockToRegion(Block *block, OpBuilder &builder,
                              llvm::SmallPtrSet<Block *, 8> &visited) {
        Location loc = llvmFunc.getLoc();

        // Guard against infinite recursion on back-edges.
        if (!visited.insert(block).second) {
            builder.create<vdrmt::NextOp>(loc, -1);
            return;
        }

        for (Operation &op : *block) {
            if (op.hasTrait<OpTrait::IsTerminator>()) break;
            convertOp(op, builder); // best-effort; missing values emit warnings
        }

        Operation *term = block->getTerminator();
        if (!term) { builder.create<vdrmt::NextOp>(loc, -1); return; }

        if (auto ret = dyn_cast<LLVM::ReturnOp>(term)) {
            int32_t s = -1;
            if (ret.getNumOperands() == 1)
                if (auto v = getConstInt(ret.getOperand(0)))
                    s = (int32_t)*v;
            builder.create<vdrmt::NextOp>(loc, s);
            return;
        }

        if (auto br = dyn_cast<LLVM::BrOp>(term)) {
            // Unconditional fall-through: tail-convert successor.
            // Use a fresh visited set copy so join-point duplication works.
            llvm::SmallPtrSet<Block *, 8> vis2(visited);
            convertBlockToRegion(br.getDest(), builder, vis2);
            return;
        }

        if (auto condBr = dyn_cast<LLVM::CondBrOp>(term)) {
            Value cond = valMap.lookup(condBr.getCondition());
            if (!cond) cond = getOrEmitValue(condBr.getCondition(), builder);
            if (!cond) { builder.create<vdrmt::NextOp>(loc, -1); return; }

            Block *T = condBr.getTrueDest();
            Block *F = condBr.getFalseDest();
            // Snapshot valMap so each branch gets an independent scope.
            // Values from dominating blocks remain visible; region-local
            // values from one sibling don't leak into the other.
            DenseMap<Value, Value> snap = valMap;
            llvm::SmallPtrSet<Block *, 8> visT(visited), visF(visited);
            builder.create<vdrmt::IfOp>(loc, cond, /*withElseRegion=*/true,
                [&, T, snap, visT](OpBuilder &b, Location l) mutable {
                    valMap = snap;
                    convertBlockToRegion(T, b, visT);
                },
                [&, F, snap, visF](OpBuilder &b, Location l) mutable {
                    valMap = snap;
                    convertBlockToRegion(F, b, visF);
                });
            valMap = snap; // discard any additions from region builds
            return;
        }

        // Unknown terminator — emit a safe fallback.
        builder.create<vdrmt::NextOp>(loc, -1);
    }

    // Handle more complex multi-block CFG where successor blocks contain ops.
    // Recursively unrolls the CFG into a nested vdrmt.if tree.
    LogicalResult convertMultiBlockCFG(LLVM::CondBrOp condBr, OpBuilder &builder) {
        Location loc = llvmFunc.getLoc();
        Value cond = valMap.lookup(condBr.getCondition());
        if (!cond) cond = getOrEmitValue(condBr.getCondition(), builder);
        if (!cond) return failure();

        Block *T = condBr.getTrueDest();
        Block *F = condBr.getFalseDest();
        DenseMap<Value, Value> snap = valMap;
        llvm::SmallPtrSet<Block *, 8> visT, visF;
        builder.create<vdrmt::IfOp>(loc, cond, /*withElseRegion=*/true,
            [&, T, snap, visT](OpBuilder &b, Location l) mutable {
                valMap = snap;
                convertBlockToRegion(T, b, visT);
            },
            [&, F, snap, visF](OpBuilder &b, Location l) mutable {
                valMap = snap;
                convertBlockToRegion(F, b, visF);
            });
        valMap = snap;
        builder.create<func::ReturnOp>(loc);
        return success();
    }
};

// ============================================================================
// File-level converter
// ============================================================================

struct LLVMIRToVDRMTFileConverter {
    std::string outputDir;

    LLVMIRToVDRMTFileConverter(StringRef dir) : outputDir(dir.str()) {}

    LogicalResult convert(StringRef llFilePath) {
        // 1. Parse LLVM IR.
        llvm::LLVMContext llvmCtx;
        llvm::SMDiagnostic err;
        auto llvmMod = llvm::parseIRFile(llFilePath, err, llvmCtx);
        if (!llvmMod) {
            llvm::errs() << "  ✗ Cannot parse LLVM IR: " << llFilePath
                         << ": " << err.getMessage() << "\n";
            return failure();
        }

        // 2. Fresh MLIRContext for import + conversion.
        MLIRContext importCtx;
        {
            DialectRegistry reg;
            mlir::registerAllDialects(reg);
            mlir::registerLLVMDialectImport(reg);
            importCtx.appendDialectRegistry(reg);
        }
        importCtx.loadDialect<LLVM::LLVMDialect>();
        importCtx.loadDialect<vdrmt::vDRMTDialect>();
        importCtx.loadDialect<func::FuncDialect>();

        // 3. Import into MLIR LLVM dialect.
        auto mlirMod = translateLLVMIRToModule(std::move(llvmMod), &importCtx);
        if (!mlirMod) {
            llvm::errs() << "  ✗ Cannot import LLVM IR to MLIR: " << llFilePath << "\n";
            return failure();
        }

        // 4. Convert each non-external llvm.func.
        int blockIdx = 0;
        for (auto llvmFunc : mlirMod->getBody()->getOps<LLVM::LLVMFuncOp>()) {
            if (llvmFunc.isExternal()) continue;
            if (failed(convertOneFunction(llvmFunc, &importCtx, blockIdx)))
                return failure();
            ++blockIdx;
        }

        llvm::outs() << "  Summary: converted " << blockIdx
                     << " function(s) to vDRMT blocks\n";
        return success();
    }

private:
    LogicalResult convertOneFunction(LLVM::LLVMFuncOp llvmFunc,
                                     MLIRContext *importCtx, int blockId) {
        std::string dirName = outputDir + "/block" + std::to_string(blockId);
        if (auto ec = llvm::sys::fs::create_directories(dirName)) {
            llvm::errs() << "  ✗ Cannot create " << dirName
                         << ": " << ec.message() << "\n";
            return failure();
        }

        // Output module lives in the same importCtx (avoids location mixing).
        OpBuilder modBuilder(importCtx);
        auto outMod = modBuilder.create<ModuleOp>(llvmFunc.getLoc());
        OpBuilder innerBuilder = OpBuilder::atBlockEnd(outMod.getBody());

        FunctionConverter conv(llvmFunc, importCtx, innerBuilder, blockId);
        if (failed(conv.convert())) {
            llvm::errs() << "  ✗ Failed to convert function '"
                         << llvmFunc.getName() << "'\n";
            return failure();
        }

        // Write vdrmt.mlir.
        std::string vdrmtFile = dirName + "/vdrmt.mlir";
        std::error_code EC;
        llvm::raw_fd_ostream out(vdrmtFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot write " << vdrmtFile << "\n";
            return failure();
        }
        outMod.print(out);
        llvm::outs() << "  ✓ block" << blockId << "/vdrmt.mlir  ("
                     << llvmFunc.getName() << ")\n";
        return success();
    }
};

// ============================================================================
// MLIR Pass wrapper
// ============================================================================

namespace {
struct LLVMIRToVDRMTPass
    : public PassWrapper<LLVMIRToVDRMTPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LLVMIRToVDRMTPass)

    LLVMIRToVDRMTPass() = default;
    LLVMIRToVDRMTPass(const LLVMIRToVDRMTPass &o) : PassWrapper(o) {}

    StringRef getArgument()    const final { return "llvm-ir-to-vdrmt"; }
    StringRef getDescription() const final {
        return "Import LLVM IR (.ll) and raise to vDRMT dialect via structural "
               "pattern matching.  Writes vdsa_output/blockN/vdrmt.mlir per "
               "function.  Use --input-ll=<path> to specify the input.";
    }

    Option<std::string> inputLL{
        *this, "input-ll",
        llvm::cl::desc("Path to the input LLVM IR (.ll) file"),
        llvm::cl::init("")
    };

    Option<std::string> outputDir{
        *this, "output-dir",
        llvm::cl::desc("Output directory for vdsa_output blocks"),
        llvm::cl::init("vdsa_output")
    };

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<vdrmt::vDRMTDialect>();
        registry.insert<func::FuncDialect>();
        registry.insert<LLVM::LLVMDialect>();
    }

    void runOnOperation() override {
        llvm::outs()
            << "\n"
            << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            << "  LLVM IR → vDRMT Pattern-Matching Pass\n"
            << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

        if (inputLL.empty()) {
            llvm::errs() << "❌ --input-ll=<path> is required\n";
            signalPassFailure();
            return;
        }

        if (!llvm::sys::fs::exists(inputLL)) {
            llvm::errs() << "❌ Input file not found: " << inputLL << "\n";
            signalPassFailure();
            return;
        }

        llvm::outs() << "  Input:  " << inputLL << "\n"
                     << "  Output: " << outputDir << "/blockN/vdrmt.mlir\n\n";

        LLVMIRToVDRMTFileConverter converter(outputDir);
        if (failed(converter.convert(inputLL))) {
            llvm::errs() << "❌ LLVM IR → vDRMT conversion failed\n";
            signalPassFailure();
            return;
        }

        llvm::outs()
            << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            << "✅ LLVM IR → vDRMT complete\n"
            << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    }
};
} // namespace

namespace mlir {
std::unique_ptr<Pass> createLLVMIRToVDRMTPass() {
    return std::make_unique<LLVMIRToVDRMTPass>();
}
void registerLLVMIRToVDRMTPass() {
    PassRegistration<LLVMIRToVDRMTPass>();
}
} // namespace mlir
