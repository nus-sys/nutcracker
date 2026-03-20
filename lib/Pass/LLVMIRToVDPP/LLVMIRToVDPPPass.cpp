// ============================================================================
// LLVMIRToVDPPPass.cpp — LLVM IR frontend: raise LLVM dialect → vDPP dialect
//
// Pipeline position:
//   .ll file → [this pass] → vdsa_output/blockN/vdpp.mlir
//                          → [VDPPToBF3DPA] → bf3dpa.mlir + arm.ll
//
// Each llvm.func in the input module becomes one vDSA block.
// The pass is an alternative to the P4HIR partition + P4HIRToVDPP path.
//
// Type mapping:
//   i1/i8/i16/i32/i64            → same builtin integer types
//   !llvm.ptr                    → !vdpp.ptr<i8>  (opaque; refined by context)
//   !llvm.struct<"n",(f0,f1,..)> → !vdpp.struct<"n",(f0,f1,..)>  (recursive)
//
// Op mapping (1:1 structural):
//   llvm.func        → func.func
//   llvm.mlir.constant → vdpp.constant
//   llvm.add/sub/mul/sdiv/udiv/and/or/xor → vdpp.add/…
//   llvm.shl/lshr/ashr → vdpp.shl/lshr/ashr
//   llvm.icmp        → vdpp.icmp
//   llvm.alloca      → vdpp.alloca  (elem_type preserved)
//   llvm.load        → vdpp.load
//   llvm.store       → vdpp.store
//   llvm.getelementptr → vdpp.getelementptr  (elem_type from base ptr)
//   llvm.zext/trunc/ptrtoint/inttoptr → vdpp.cast
//   llvm.br          → vdpp.br
//   llvm.cond_br     → vdpp.cond_br
//   llvm.return i32  → vdpp.return {successor=N}  (constant return value)
// ============================================================================

#include "Pass/LLVMIRToVDPPPass.h"

#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Parser/Parser.h"
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
#include <string>

using namespace mlir;

// ============================================================================
// Type conversion: LLVM dialect types → vDPP types
// ============================================================================

static Type convertLLVMType(Type t, MLIRContext *ctx);

/// Recursively convert !llvm.struct → !vdpp.struct.
static Type convertStructType(LLVM::LLVMStructType st, MLIRContext *ctx) {
    SmallVector<Type> fields;
    for (Type f : st.getBody()) {
        Type c = convertLLVMType(f, ctx);
        if (!c) return nullptr;
        fields.push_back(c);
    }
    if (st.isIdentified())
        return vdpp::StructType::get(ctx, st.getName(), fields, st.isPacked());
    return vdpp::StructType::get(ctx, fields, st.isPacked());
}

static Type convertLLVMType(Type t, MLIRContext *ctx) {
    // Builtin integer types are shared between LLVM and vDPP dialects.
    if (isa<IntegerType>(t))
        return t;
    if (isa<LLVM::LLVMPointerType>(t))
        // Opaque pointer: use i8 as placeholder element type.
        return vdpp::PointerType::get(ctx, IntegerType::get(ctx, 8));
    if (auto st = dyn_cast<LLVM::LLVMStructType>(t))
        return convertStructType(st, ctx);
    // Float types, vector types, etc. — not supported in vDPP.
    return nullptr;
}

// ============================================================================
// Compute GEP result element type from base elem type + constant indices
// ============================================================================

/// Given the element type that a GEP indexes INTO and the raw constant
/// indices (INT_MIN = dynamic), return the element type pointed to by the
/// result.  Returns nullptr if the result cannot be determined statically.
static Type computeGEPResultElemType(Type baseElemTy,
                                     ArrayRef<int32_t> rawIndices) {
    Type cur = baseElemTy;
    // Index 0 selects the instance (doesn't descend into the type).
    // Indices 1..N-1 descend into aggregates.
    for (int i = 1; i < (int)rawIndices.size(); ++i) {
        int32_t idx = rawIndices[i];
        if (idx == std::numeric_limits<int32_t>::min())
            return nullptr; // dynamic index, unknown statically
        if (auto st = dyn_cast<LLVM::LLVMStructType>(cur)) {
            if (idx < 0 || idx >= (int)st.getBody().size())
                return nullptr;
            cur = st.getBody()[idx];
        } else if (auto at = dyn_cast<LLVM::LLVMArrayType>(cur)) {
            cur = at.getElementType();
        } else {
            return nullptr; // scalar, can't index further
        }
    }
    return cur;
}

// ============================================================================
// ICmpPredicate mapping
// ============================================================================

static vdpp::ICmpPredicate llvmPredToVDPP(LLVM::ICmpPredicate p) {
    switch (p) {
    case LLVM::ICmpPredicate::eq:  return vdpp::ICmpPredicate::eq;
    case LLVM::ICmpPredicate::ne:  return vdpp::ICmpPredicate::ne;
    case LLVM::ICmpPredicate::slt: return vdpp::ICmpPredicate::slt;
    case LLVM::ICmpPredicate::sle: return vdpp::ICmpPredicate::sle;
    case LLVM::ICmpPredicate::sgt: return vdpp::ICmpPredicate::sgt;
    case LLVM::ICmpPredicate::sge: return vdpp::ICmpPredicate::sge;
    case LLVM::ICmpPredicate::ult: return vdpp::ICmpPredicate::ult;
    case LLVM::ICmpPredicate::ule: return vdpp::ICmpPredicate::ule;
    case LLVM::ICmpPredicate::ugt: return vdpp::ICmpPredicate::ugt;
    case LLVM::ICmpPredicate::uge: return vdpp::ICmpPredicate::uge;
    }
    return vdpp::ICmpPredicate::eq;
}

// ============================================================================
// Per-op conversion (direct walk, no ConversionPattern framework)
// ============================================================================

/// Convert one LLVM dialect op to a vDPP op.
/// `mapping` maps LLVM values → already-created vDPP values.
static LogicalResult convertOp(Operation &op, OpBuilder &builder,
                                IRMapping &mapping, MLIRContext *ctx) {
    auto loc = op.getLoc();

    // ── Constants ──────────────────────────────────────────────────────────
    if (auto c = dyn_cast<LLVM::ConstantOp>(op)) {
        Type resTy = convertLLVMType(c.getType(), ctx);
        if (!resTy) {
            llvm::errs() << "  ✗ LLVMIRToVDPP: cannot convert type of constant: "
                         << c.getType() << "\n";
            return failure();
        }
        auto newOp = builder.create<vdpp::ConstantOp>(loc, resTy, c.getValue());
        mapping.map(c.getResult(), newOp.getRes());
        return success();
    }

    // ── Arithmetic ─────────────────────────────────────────────────────────
    auto mapBinOp = [&](Value lhs, Value rhs, auto createFn) -> LogicalResult {
        Value l = mapping.lookup(lhs), r = mapping.lookup(rhs);
        if (!l || !r) return failure();
        Value res = createFn(l, r);
        mapping.map(op.getResult(0), res);
        return success();
    };

#define MAP_BINOP(LLVMOp, VDPPOp)                                              \
    if (auto x = dyn_cast<LLVM::LLVMOp>(op)) {                                \
        return mapBinOp(x.getLhs(), x.getRhs(),                                \
            [&](Value l, Value r) {                                            \
                return builder.create<vdpp::VDPPOp>(loc, l.getType(), l, r)   \
                              .getResult();                                     \
            });                                                                 \
    }
    MAP_BINOP(AddOp,  AddOp)
    MAP_BINOP(SubOp,  SubOp)
    MAP_BINOP(MulOp,  MulOp)
    MAP_BINOP(SDivOp, DivOp)
    MAP_BINOP(UDivOp, UDivOp)
    MAP_BINOP(AndOp,  AndOp)
    MAP_BINOP(OrOp,   OrOp)
    MAP_BINOP(XOrOp,  XorOp)
#undef MAP_BINOP

    // Shift ops have different operand names.
#define MAP_SHIFTOP(LLVMOp, VDPPOp)                                            \
    if (auto x = dyn_cast<LLVM::LLVMOp>(op)) {                                \
        Value v = mapping.lookup(x.getLhs());                                  \
        Value a = mapping.lookup(x.getRhs());                                  \
        if (!v || !a) return failure();                                        \
        auto res = builder.create<vdpp::VDPPOp>(loc, v.getType(), v, a);      \
        mapping.map(x.getResult(), res.getResult());                           \
        return success();                                                       \
    }
    MAP_SHIFTOP(ShlOp,  ShlOp)
    MAP_SHIFTOP(LShrOp, LShrOp)
    MAP_SHIFTOP(AShrOp, AShrOp)
#undef MAP_SHIFTOP

    // ── ICmp ───────────────────────────────────────────────────────────────
    if (auto icmp = dyn_cast<LLVM::ICmpOp>(op)) {
        Value l = mapping.lookup(icmp.getLhs());
        Value r = mapping.lookup(icmp.getRhs());
        if (!l || !r) return failure();
        auto pred = llvmPredToVDPP(icmp.getPredicate());
        auto res = builder.create<vdpp::ICmpOp>(loc, pred, l, r);
        mapping.map(icmp.getResult(), res.getResult());
        return success();
    }

    // ── Memory ─────────────────────────────────────────────────────────────
    if (auto alloca = dyn_cast<LLVM::AllocaOp>(op)) {
        Type elemTy = convertLLVMType(alloca.getElemType(), ctx);
        if (!elemTy) {
            llvm::errs() << "  ✗ LLVMIRToVDPP: cannot convert alloca elem type: "
                         << alloca.getElemType() << "\n";
            return failure();
        }
        auto ptrTy = vdpp::PointerType::get(ctx, elemTy);
        auto res = builder.create<vdpp::AllocaOp>(
            loc, ptrTy, mlir::Value{},
            (uint64_t)alloca.getAlignment().value_or(1));
        mapping.map(alloca.getResult(), res.getResult());
        return success();
    }

    if (auto load = dyn_cast<LLVM::LoadOp>(op)) {
        Value ptr = mapping.lookup(load.getAddr());
        if (!ptr) return failure();
        // Result type: from converted LLVM result type (not pointer element).
        Type resTy = convertLLVMType(load.getRes().getType(), ctx);
        if (!resTy) {
            llvm::errs() << "  ✗ LLVMIRToVDPP: cannot convert load result type: "
                         << load.getRes().getType() << "\n";
            return failure();
        }
        // Ensure the pointer has the right element type.
        auto ptrTy = vdpp::PointerType::get(ctx, resTy);
        if (ptr.getType() != ptrTy) {
            ptr = builder.create<vdpp::CastOp>(loc, ptrTy, ptr).getResult();
        }
        auto res = builder.create<vdpp::LoadOp>(
            loc, resTy, ptr,
            builder.getI64IntegerAttr(load.getAlignment().value_or(0)));
        mapping.map(load.getResult(), res.getResult());
        return success();
    }

    if (auto store = dyn_cast<LLVM::StoreOp>(op)) {
        Value val = mapping.lookup(store.getValue());
        Value ptr = mapping.lookup(store.getAddr());
        if (!val || !ptr) return failure();
        // Ensure pointer elem type matches value type.
        auto ptrTy = vdpp::PointerType::get(ctx, val.getType());
        if (ptr.getType() != ptrTy)
            ptr = builder.create<vdpp::CastOp>(loc, ptrTy, ptr).getResult();
        builder.create<vdpp::StoreOp>(
            loc, val, ptr,
            builder.getI64IntegerAttr(store.getAlignment().value_or(0)));
        return success();
    }

    if (auto gep = dyn_cast<LLVM::GEPOp>(op)) {
        Value base = mapping.lookup(gep.getBase());
        if (!base) return failure();

        // Collect dynamic index operands.
        SmallVector<Value> indices;
        int dynIdx = 0;
        for (int32_t raw : gep.getRawConstantIndices()) {
            if (raw == std::numeric_limits<int32_t>::min()) {
                // Dynamic: take the next operand after base.
                Value idx = mapping.lookup(gep.getDynamicIndices()[dynIdx++]);
                if (!idx) return failure();
                indices.push_back(idx);
            } else {
                // Constant: materialise as vdpp.constant i32.
                indices.push_back(builder.create<vdpp::ConstantOp>(
                    loc, builder.getI32Type(),
                    builder.getI32IntegerAttr(raw)).getRes());
            }
        }

        // Compute result element type from base elem type + constant indices.
        Type resultElemTy = nullptr;
        if (Type baseElem = convertLLVMType(gep.getElemType(), ctx))
            resultElemTy = convertLLVMType(
                computeGEPResultElemType(gep.getElemType(),
                                         gep.getRawConstantIndices()),
                ctx);
        if (!resultElemTy)
            resultElemTy = IntegerType::get(ctx, 8); // fallback: i8

        // Ensure base pointer has the correct (converted) elem type.
        Type baseElemConverted = convertLLVMType(gep.getElemType(), ctx);
        if (!baseElemConverted)
            baseElemConverted = IntegerType::get(ctx, 8);
        auto basePtrTy = vdpp::PointerType::get(ctx, baseElemConverted);
        if (base.getType() != basePtrTy)
            base = builder.create<vdpp::CastOp>(loc, basePtrTy, base).getResult();

        auto resPtrTy = vdpp::PointerType::get(ctx, resultElemTy);
        auto res = builder.create<vdpp::GetElementPtrOp>(
            loc, resPtrTy, base, indices);
        mapping.map(gep.getResult(), res.getResult());
        return success();
    }

    // ── Casts ──────────────────────────────────────────────────────────────
    auto mapCast = [&](Value src) -> LogicalResult {
        Value s = mapping.lookup(src);
        if (!s) return failure();
        Type dstTy = convertLLVMType(op.getResult(0).getType(), ctx);
        if (!dstTy) return failure();
        auto res = builder.create<vdpp::CastOp>(loc, dstTy, s);
        mapping.map(op.getResult(0), res.getResult());
        return success();
    };
    if (auto x = dyn_cast<LLVM::ZExtOp>(op))   return mapCast(x.getArg());
    if (auto x = dyn_cast<LLVM::SExtOp>(op))   return mapCast(x.getArg());
    if (auto x = dyn_cast<LLVM::TruncOp>(op))  return mapCast(x.getArg());
    if (auto x = dyn_cast<LLVM::PtrToIntOp>(op)) return mapCast(x.getArg());
    if (auto x = dyn_cast<LLVM::IntToPtrOp>(op)) return mapCast(x.getArg());
    if (auto x = dyn_cast<LLVM::BitcastOp>(op)) return mapCast(x.getArg());

    // ── Control flow ───────────────────────────────────────────────────────
    if (auto br = dyn_cast<LLVM::BrOp>(op)) {
        Block *dest = mapping.lookup(br.getDest());
        if (!dest) return failure();
        SmallVector<Value> destOps;
        for (Value v : br.getDestOperands())
            destOps.push_back(mapping.lookup(v));
        builder.create<vdpp::BranchOp>(loc, dest, destOps);
        return success();
    }

    if (auto cbr = dyn_cast<LLVM::CondBrOp>(op)) {
        Value cond = mapping.lookup(cbr.getCondition());
        if (!cond) return failure();
        Block *trueDest  = mapping.lookup(cbr.getTrueDest());
        Block *falseDest = mapping.lookup(cbr.getFalseDest());
        if (!trueDest || !falseDest) return failure();
        SmallVector<Value> trueOps, falseOps;
        for (Value v : cbr.getTrueDestOperands())
            trueOps.push_back(mapping.lookup(v));
        for (Value v : cbr.getFalseDestOperands())
            falseOps.push_back(mapping.lookup(v));
        builder.create<vdpp::CondBranchOp>(
            loc, TypeRange{}, cond,
            trueOps, falseOps,
            trueDest, falseDest);
        return success();
    }

    if (auto ret = dyn_cast<LLVM::ReturnOp>(op)) {
        // Convention: an i32 return value is the vDSA successor block ID.
        std::optional<int32_t> succ;
        if (ret.getNumOperands() == 1) {
            Value retVal = mapping.lookup(ret.getOperand(0));
            // Try to extract a constant successor.
            if (retVal) {
                if (auto defOp = retVal.getDefiningOp<vdpp::ConstantOp>()) {
                    if (auto iAttr =
                            dyn_cast<IntegerAttr>(defOp.getValue()))
                        succ = (int32_t)iAttr.getInt();
                }
            }
        }
        if (succ.has_value())
            builder.create<vdpp::ReturnOp>(loc, *succ);
        else
            builder.create<vdpp::ReturnOp>(loc);
        return success();
    }

    // Ignore debug intrinsics and other metadata ops.
    if (op.hasTrait<OpTrait::IsTerminator>()) {
        llvm::errs() << "  ✗ LLVMIRToVDPP: unhandled terminator: "
                     << op.getName() << "\n";
        return failure();
    }
    // Non-terminator ops with no result (e.g. lifetime markers) — skip.
    if (op.getNumResults() == 0)
        return success();

    llvm::errs() << "  ✗ LLVMIRToVDPP: unhandled op: " << op.getName() << "\n";
    return failure();
}

// ============================================================================
// Function conversion
// ============================================================================

static LogicalResult convertFunction(LLVM::LLVMFuncOp llvmFunc,
                                     OpBuilder &modBuilder,
                                     MLIRContext *ctx,
                                     int blockId) {
    auto loc = llvmFunc.getLoc();

    // Build vDPP argument types from LLVM func type.
    auto llvmFnTy = llvmFunc.getFunctionType();
    SmallVector<Type> argTypes;
    for (Type t : llvmFnTy.getParams()) {
        Type c = convertLLVMType(t, ctx);
        if (!c) {
            llvm::errs() << "  ✗ LLVMIRToVDPP: cannot convert arg type " << t << "\n";
            return failure();
        }
        argTypes.push_back(c);
    }
    // vDPP functions are always void-returning; successor is in vdpp.return.
    auto vdppFnTy = FunctionType::get(ctx, argTypes, {});

    auto funcOp = modBuilder.create<func::FuncOp>(
        loc, llvmFunc.getName(), vdppFnTy);
    funcOp->setAttr("vdpp.block_id",
                    modBuilder.getI32IntegerAttr(blockId));

    // Propagate successor list from function attributes if present.
    if (auto succ = llvmFunc->getAttrOfType<ArrayAttr>("vdpp.successors"))
        funcOp->setAttr("vdpp.successors", succ);

    // Build a block-for-block mapping.
    Region &llvmRegion = llvmFunc.getBody();
    if (llvmRegion.empty()) {
        // Declaration (no body) — skip.
        funcOp.erase();
        return success();
    }

    Region &vdppRegion = funcOp.getBody();
    IRMapping mapping;

    // Create corresponding blocks (with vDPP arg types).
    for (Block &llvmBlock : llvmRegion) {
        Block *vdppBlock = &vdppRegion.emplaceBlock();
        for (BlockArgument arg : llvmBlock.getArguments()) {
            Type c = convertLLVMType(arg.getType(), ctx);
            if (!c) {
                llvm::errs() << "  ✗ LLVMIRToVDPP: cannot convert block arg type "
                             << arg.getType() << "\n";
                return failure();
            }
            BlockArgument newArg = vdppBlock->addArgument(c, arg.getLoc());
            mapping.map(arg, newArg);
        }
        mapping.map(&llvmBlock, vdppBlock);
    }

    // Translate ops block by block.
    OpBuilder builder(ctx);
    for (Block &llvmBlock : llvmRegion) {
        Block *vdppBlock = mapping.lookup(&llvmBlock);
        builder.setInsertionPointToEnd(vdppBlock);
        for (Operation &op : llvmBlock) {
            if (failed(convertOp(op, builder, mapping, ctx)))
                return failure();
        }
    }
    return success();
}

// ============================================================================
// LLVM IR file import + conversion driver
// ============================================================================

struct LLVMIRToVDPPConverter {
    std::string outputDir;

    LLVMIRToVDPPConverter(MLIRContext * /*unused*/, StringRef outputDir)
        : outputDir(outputDir.str()) {}

    /// Import a .ll file, convert each llvm.func to a vdpp.mlir block.
    LogicalResult convert(StringRef llFilePath) {
        // 1. Parse LLVM IR using LLVM's IR reader.
        llvm::LLVMContext llvmCtx;
        llvm::SMDiagnostic err;
        auto llvmMod = llvm::parseIRFile(llFilePath, err, llvmCtx);
        if (!llvmMod) {
            llvm::errs() << "  ✗ Cannot parse LLVM IR file: " << llFilePath
                         << ": " << err.getMessage() << "\n";
            return failure();
        }

        // 2. Create a fresh MLIRContext (not in the PM's multi-threaded mode)
        //    so that translateLLVMIRToModule can safely load dialects.
        MLIRContext importCtx;
        {
            DialectRegistry reg;
            mlir::registerAllDialects(reg);
            mlir::registerLLVMDialectImport(reg);
            importCtx.appendDialectRegistry(reg);
        }
        importCtx.loadDialect<LLVM::LLVMDialect>();
        importCtx.loadDialect<vdpp::vDPPDialect>();
        importCtx.loadDialect<func::FuncDialect>();

        // 3. Import into MLIR LLVM dialect.
        auto mlirMod = translateLLVMIRToModule(std::move(llvmMod), &importCtx);
        if (!mlirMod) {
            llvm::errs() << "  ✗ Cannot import LLVM IR to MLIR: "
                         << llFilePath << "\n";
            return failure();
        }

        // 4. Convert each llvm.func to a vdpp block.
        int blockId = 0;
        for (auto llvmFunc :
             mlirMod->getBody()->getOps<LLVM::LLVMFuncOp>()) {
            if (llvmFunc.isExternal()) continue; // skip declarations

            if (failed(convertOneFunction(llvmFunc, &importCtx, blockId)))
                return failure();
            ++blockId;
        }

        llvm::outs() << "  Summary: converted " << blockId
                     << " function(s) to vDPP blocks\n";
        return success();
    }

private:
    LogicalResult convertOneFunction(LLVM::LLVMFuncOp llvmFunc,
                                     MLIRContext *importCtx, int blockId) {
        std::string dirName = outputDir + "/block" + std::to_string(blockId);
        // Create the block directory if it doesn't exist.
        if (auto ec = llvm::sys::fs::create_directories(dirName)) {
            llvm::errs() << "  ✗ Cannot create directory " << dirName
                         << ": " << ec.message() << "\n";
            return failure();
        }

        // Build a vDPP module in the same importCtx to avoid location mixing.
        OpBuilder builder(importCtx);
        auto outMod = builder.create<ModuleOp>(llvmFunc.getLoc());
        OpBuilder modBuilder = OpBuilder::atBlockEnd(outMod.getBody());

        if (failed(convertFunction(llvmFunc, modBuilder, importCtx, blockId))) {
            llvm::errs() << "  ✗ Failed to convert function '"
                         << llvmFunc.getName() << "'\n";
            return failure();
        }

        // Write vdpp.mlir.
        std::string vdppFile = dirName + "/vdpp.mlir";
        std::error_code EC;
        llvm::raw_fd_ostream out(vdppFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot write " << vdppFile << "\n";
            return failure();
        }
        outMod.print(out);
        llvm::outs() << "  ✓ block" << blockId << "/vdpp.mlir  ("
                     << llvmFunc.getName() << ")\n";
        return success();
    }
};

// ============================================================================
// MLIR Pass wrapper
// ============================================================================

namespace {

struct LLVMIRToVDPPPass
    : public PassWrapper<LLVMIRToVDPPPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LLVMIRToVDPPPass)

    LLVMIRToVDPPPass() = default;
    LLVMIRToVDPPPass(const LLVMIRToVDPPPass &o) : PassWrapper(o) {}

    StringRef getArgument()    const final { return "llvm-ir-to-vdpp"; }
    StringRef getDescription() const final {
        return "Import LLVM IR (.ll) and raise to vDPP dialect; "
               "writes vdsa_output/blockN/vdpp.mlir per function. "
               "Use --input-ll=<path> to specify the input file.";
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
        registry.insert<vdpp::vDPPDialect>();
        registry.insert<func::FuncDialect>();
        registry.insert<LLVM::LLVMDialect>();
    }

    void runOnOperation() override {
        llvm::outs() << "\n"
            << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            << "  LLVM IR → vDPP Frontend Pass\n"
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
                     << "  Output: " << outputDir << "/blockN/vdpp.mlir\n\n";

        auto *ctx = &getContext();
        LLVMIRToVDPPConverter converter(ctx, outputDir);
        if (failed(converter.convert(inputLL))) {
            llvm::errs() << "❌ LLVM IR → vDPP conversion failed\n";
            signalPassFailure();
            return;
        }

        llvm::outs()
            << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            << "✅ LLVM IR → vDPP complete\n"
            << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    }
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

std::unique_ptr<Pass> createLLVMIRToVDPPPass() {
    return std::make_unique<LLVMIRToVDPPPass>();
}

void registerLLVMIRToVDPPPass() {
    PassRegistration<LLVMIRToVDPPPass>();
}

} // namespace mlir
