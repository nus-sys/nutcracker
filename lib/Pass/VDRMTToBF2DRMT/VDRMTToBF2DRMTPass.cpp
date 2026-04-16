// ============================================================================
// File: lib/Pass/VDRMTToBF2DRMT/VDRMTToBF2DRMTPass.cpp
// vDRMT → bf2drmt Fine-Grained Lowering Pass (DialEgg-based)
//
// This pass implements the fine-grained, op-level lowering from the virtual
// dRMT (vDRMT) dialect to the BF3 NIC ASIC target dialect (bf2drmt) using
// the DialEgg equality saturation infrastructure.
//
// Workflow:
//   Phase 1 — op-level conversion via DialEgg:
//     1. Eggify every op in each vdrmt func.func using Egglog::eggifyOperation.
//     2. Inject the eggified ops into the vdrmt_to_bf2drmt.egg template at the
//        ";; OPS HERE ;;" marker.
//     3. Execute the egglog binary; equality saturation rewrites vdrmt ops to
//        bf2drmt ops using the rules in the template.
//     4. Read the extracted results from the ";; EXTRACTS HERE ;;" section and
//        call Egglog::parseOperation to materialise the bf2drmt MLIR ops,
//        replacing the original vdrmt ops in place.
//
//   Phase 2 — structural conversion (per-function):
//     Each func.func wrapper is replaced by a bf2drmt.pipe op.
//     For functions with a vdrmt.cmp → vdrmt.if pattern (table-based routing):
//       - The bf2drmt read chain that produces the compared value is placed in a
//         bf2drmt.table_key region, terminated by bf2drmt.match_key.
//       - The then-successor becomes a bf2drmt.pipe_action @_hit { next N }.
//       - The else-successor becomes a bf2drmt.pipe_action @_miss { next N }.
//     For action-only functions (no conditional routing):
//       - All ops are placed in a bf2drmt.pipe_action @_default body.
//       - The vdrmt.next successor becomes a bf2drmt.next terminator.
// ============================================================================

#include <fstream>
#include <map>
#include <string>

#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/AsmParser/AsmParser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "Pass/VDRMTToBF2DRMTPass.h"
#include "Pass/BF2DRMTToDocaFlowPass.h"
#include "Pass/VFFSCoarseGrainedPass.h"
#include "Pass/Egglog.h"
#include "Pass/Utils.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTDialect.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOps.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.h"

using namespace mlir;
using namespace mlir::vdrmt;

namespace {

// ============================================================================
// Custom EgglogCustomDefs for vDRMT / bf2drmt types
// ============================================================================

/// Egglog string literals use double quotes but do NOT support backslash
/// escaping (e.g. \" is invalid).  MLIR struct/header type strings contain
/// double-quote characters (e.g. !vdrmt.struct<"main_headers_t", ...>).
///
/// We encode them by replacing " with the placeholder __Q__ so the result
/// is safe to embed inside an egglog double-quoted string literal.
static std::string escapeEgglogString(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') r += "__Q__";
        else          r += c;
    }
    return r;
}

/// Reverse of escapeEgglogString — __Q__ → ".
static std::string unescapeEgglogString(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s.compare(i, 5, "__Q__") == 0) {
            r += '"';
            i += 5;
        } else {
            r += s[i++];
        }
    }
    return r;
}

/// Recursively convert vdrmt types to their bf2drmt equivalents.
///   vdrmt::ReferenceType → bf2drmt::ReferenceType
///   vdrmt::StructType    → bf2drmt::StructType
///   vdrmt::HeaderType    → bf2drmt::HeaderType
///   everything else      → unchanged
static mlir::Type convertToBF2DRMTType(mlir::Type type) {
    if (auto refTy = mlir::dyn_cast<vdrmt::ReferenceType>(type))
        return bf2drmt::ReferenceType::get(
            refTy.getContext(), convertToBF2DRMTType(refTy.getObjectType()));
    if (auto sTy = mlir::dyn_cast<vdrmt::StructType>(type)) {
        llvm::SmallVector<vdrmt::FieldInfo> fields;
        for (const auto &f : sTy.getElements())
            fields.emplace_back(f.name, convertToBF2DRMTType(f.type), f.annotations);
        return bf2drmt::StructType::get(sTy.getContext(), sTy.getName(), fields,
                                        sTy.getAnnotations());
    }
    if (auto hTy = mlir::dyn_cast<vdrmt::HeaderType>(type)) {
        llvm::SmallVector<vdrmt::FieldInfo> fields;
        for (const auto &f : hTy.getElements())
            fields.emplace_back(f.name, convertToBF2DRMTType(f.type), f.annotations);
        return bf2drmt::HeaderType::get(hTy.getContext(), hTy.getName(), fields,
                                        hTy.getAnnotations());
    }
    return type;
}

/// Build the EgglogCustomDefs that handle vdrmt.ref / vdrmt.struct /
/// vdrmt.header (and their bf2drmt counterparts).
static EgglogCustomDefs buildVDRMTCustomDefs(MLIRContext *ctx) {
    EgglogCustomDefs defs;

    // ── Type stringifiers ─────────────────────────────────────────────────

    // vdrmt.ref  →  (vdrmt_ref <inner_type>)
    defs.typeStringifiers["vdrmt.ref"] =
        [](mlir::Type type, Egglog &egglog) -> std::vector<std::string> {
            auto refType = mlir::cast<vdrmt::ReferenceType>(type);
            return {"vdrmt_ref", egglog.eggifyType(refType.getObjectType())};
        };

    // vdrmt.struct  →  (vdrmt_struct "escaped_mlir_type_string")
    defs.typeStringifiers["vdrmt.struct"] =
        [](mlir::Type type, Egglog &) -> std::vector<std::string> {
            std::string typeStr;
            llvm::raw_string_ostream os(typeStr);
            type.print(os);
            os.flush();
            return {"vdrmt_struct", "\"" + escapeEgglogString(typeStr) + "\""};
        };

    // vdrmt.header  →  (vdrmt_header "escaped_mlir_type_string")
    defs.typeStringifiers["vdrmt.header"] =
        [](mlir::Type type, Egglog &) -> std::vector<std::string> {
            std::string typeStr;
            llvm::raw_string_ostream os(typeStr);
            type.print(os);
            os.flush();
            return {"vdrmt_header", "\"" + escapeEgglogString(typeStr) + "\""};
        };

    // ── Type parsers ──────────────────────────────────────────────────────

    // Parse a type from the egglog escaped string:
    //   split[1]  =  "\"!vdrmt.struct<\\\"name\\\", ...>\""  (with outer quotes)
    auto parseTypeFromEscapedStr =
        [ctx](const std::vector<std::string> &split, Egglog &) -> mlir::Type {
            const std::string &raw = split[1];
            std::string inner = (raw.size() >= 2 &&
                                 raw.front() == '"' && raw.back() == '"')
                                    ? raw.substr(1, raw.size() - 2)
                                    : raw;
            std::string typeStr = unescapeEgglogString(inner);
            mlir::Type parsed = mlir::parseType(typeStr, ctx);
            // The stored string was produced from a vdrmt type — convert to bf2drmt.
            return convertToBF2DRMTType(parsed);
        };

    defs.typeParsers["vdrmt_struct"]   = parseTypeFromEscapedStr;
    defs.typeParsers["vdrmt_header"]   = parseTypeFromEscapedStr;
    defs.typeParsers["bf2drmt_struct"] = parseTypeFromEscapedStr;
    defs.typeParsers["bf2drmt_header"] = parseTypeFromEscapedStr;

    // vdrmt_ref  →  vdrmt::ReferenceType
    auto parseVdrmtRef =
        [ctx](const std::vector<std::string> &split, Egglog &egglog) -> mlir::Type {
            mlir::Type inner = egglog.parseType(split[1]);
            return vdrmt::ReferenceType::get(ctx, inner);
        };

    // bf2drmt_ref  →  bf2drmt::ReferenceType
    auto parseRef =
        [ctx](const std::vector<std::string> &split, Egglog &egglog) -> mlir::Type {
            mlir::Type inner = egglog.parseType(split[1]);
            return bf2drmt::ReferenceType::get(ctx, inner);
        };

    defs.typeParsers["vdrmt_ref"]   = parseVdrmtRef;
    defs.typeParsers["bf2drmt_ref"] = parseRef;

    return defs;
}

// ============================================================================
// DialEgg helpers: load supported ops from egg template, run egglog
// ============================================================================

/// Read the egg template file and build the supportedOps map.
static std::map<std::string, EgglogOpDef>
loadSupportedOps(const std::string &eggFilePath) {
    std::map<std::string, EgglogOpDef> ops;
    std::ifstream f(eggFilePath);
    std::string line;
    while (std::getline(f, line)) {
        if (EgglogOpDef::isOpFunction(line)) {
            EgglogOpDef def = EgglogOpDef::parseOpFunction(line);
            // fullName is e.g. "bf2drmt_struct_extract_ref"
            // parseOperation does:
            //   (1) find(opName)   — key uses underscores: "bf2drmt_struct_extract_ref"
            //   (2) replace _ → .  — key becomes:          "bf2drmt.struct.extract.ref"
            //   (3) at(opName)     — must also exist
            // So register under both forms.
            std::string underscoreKey = def.fullName;  // e.g. "bf2drmt_struct_extract_ref"
            std::string dotKey = underscoreKey;
            std::replace(dotKey.begin(), dotKey.end(), '_', '.');
            ops.emplace(underscoreKey, def);
            ops.emplace(dotKey, def);
        }
    }
    return ops;
}

/// Inject eggified ops and extract directives into the egg template, write the
/// result to opsEggFile, then invoke the egglog binary.
static LogicalResult runEgglogOnFile(
    const std::string &eggFilePath,
    const std::vector<EggifiedOp *> &eggifiedBlock,
    const std::string &blockName,
    const std::string &opsEggFile,
    const std::string &extractFile,
    const std::string &logFile) {

    std::ifstream eggFile(eggFilePath);
    if (!eggFile.is_open()) {
        llvm::errs() << "  ✗ Cannot open egg template: " << eggFilePath << "\n";
        return failure();
    }

    std::vector<std::string> lines;
    bool insertedOps = false, insertedExtracts = false;
    std::string line;

    while (std::getline(eggFile, line)) {
        lines.push_back(line);

        if (!insertedOps && line == ";; OPS HERE ;;") {
            lines.push_back("; " + blockName);
            for (const EggifiedOp *op : eggifiedBlock)
                lines.push_back(op->egglogLet());
            insertedOps = true;
        } else if (!insertedExtracts && line == ";; EXTRACTS HERE ;;") {
            for (const EggifiedOp *op : eggifiedBlock)
                if (op->shouldBeExtracted())
                    lines.push_back("(extract " + op->getPrintId() + ")");
            insertedExtracts = true;
        }
    }
    eggFile.close();

    std::ofstream out(opsEggFile);
    if (!out.is_open()) {
        llvm::errs() << "  ✗ Cannot write ops.egg: " << opsEggFile << "\n";
        return failure();
    }
    for (const std::string &l : lines)
        out << l << "\n";
    out.close();

    // Invoke egglog.
    // Resolution order:
    //   1. $EGGLOG env var (treated as a directory: $EGGLOG/egglog)
    //   2. third_party/egglog/target/release/egglog relative to CWD
    //   3. "egglog" on PATH
    const char *egglogEnv = std::getenv("EGGLOG");
    std::string egglogBin;
    if (egglogEnv) {
        egglogBin = std::string(egglogEnv) + "/egglog";
    } else if (llvm::sys::fs::exists(
                   "third_party/egglog/target/release/egglog")) {
        egglogBin = "third_party/egglog/target/release/egglog";
    } else {
        egglogBin = "egglog";
    }
    std::string cmd = egglogBin + " " + opsEggFile +
                      " > " + extractFile + " 2> " + logFile;

    llvm::outs() << "  Running egglog: " << cmd << "\n";
    int ret = std::system(cmd.c_str());

    printFileContents(logFile);

    if (ret != 0) {
        llvm::errs() << "  ✗ egglog exited with code " << ret
                     << ". See: " << logFile << "\n";
        return failure();
    }
    return success();
}

/// Run DialEgg op-level conversion on a single func.func:
///   eggify → inject into template → execute egglog → parse back → replace ops.
static LogicalResult runDialEggOnFunction(
    func::FuncOp funcOp,
    const std::string &eggFilePath,
    const std::string &vdrmtFilePath,
    const std::map<std::string, EgglogOpDef> &supportedOps,
    const EgglogCustomDefs &customDefs) {

    MLIRContext *ctx = funcOp.getContext();
    Egglog egglog(*ctx, customDefs, supportedOps);

    // Register function block arguments as opaque values.
    for (mlir::Value arg : funcOp.getArguments())
        egglog.eggifyValue(arg);

    // Eggify all ops in the function body.
    for (mlir::Block &block : funcOp.getFunctionBody().getBlocks())
        for (mlir::Operation &op : block.getOperations())
            egglog.eggifyOperation(&op);

    // Derive file paths.
    std::string base      = vdrmtFilePath.substr(0, vdrmtFilePath.find(".mlir"));
    std::string extractFile = base + "-egglog-extract.log";
    std::string logFile     = base + "-egglog.log";
    std::string opsEggFile  = base + ".ops.egg";

    if (failed(runEgglogOnFile(eggFilePath, egglog.eggifiedBlock,
                               funcOp.getName().str(),
                               opsEggFile, extractFile, logFile)))
        return failure();

    // Parse results back and replace the original ops.
    std::ifstream file(extractFile);
    if (!file.is_open()) {
        llvm::errs() << "  ✗ Cannot open extract file: " << extractFile << "\n";
        return failure();
    }

    for (const EggifiedOp *eggOp : egglog.eggifiedBlock) {
        if (!eggOp->shouldBeExtracted())
            continue;

        std::string extractLine;
        if (!std::getline(file, extractLine)) {
            llvm::errs() << "  ✗ Unexpected end of egglog extract\n";
            file.close();
            return failure();
        }

        mlir::Operation *prevOp = eggOp->mlirOp;
        mlir::OpBuilder builder(prevOp);
        mlir::Operation *newOp = egglog.parseOperation(extractLine, builder);

        if (newOp == nullptr) {
            mlir::Value value = egglog.parseValue(extractLine);
            prevOp->getResult(0).replaceAllUsesWith(value);
            prevOp->erase();
        } else if (newOp != prevOp) {
            prevOp->replaceAllUsesWith(newOp);
            prevOp->erase();
        }
    }
    file.close();

    // ── Dead-code elimination: erase ops made dead by the replacement ────────
    // After extraction, the original vdrmt ops that provided operands to the
    // now-erased root ops have no remaining users.  Only erase ops that
    // produce results (use_empty() is trivially true for 0-result ops like
    // assign/next, and those must be preserved as side effects).
    bool anyErased = true;
    while (anyErased) {
        anyErased = false;
        for (mlir::Block &block : funcOp.getFunctionBody().getBlocks()) {
            for (auto it = block.begin(); it != block.end(); ) {
                mlir::Operation &op = *it++;
                if (op.getNumResults() > 0 && op.use_empty()) {
                    op.erase();
                    anyErased = true;
                }
            }
        }
    }

    // ── Lower remaining vdrmt.variable → bf2drmt.variable ───────────────────
    // vdrmt.variable is encoded as an opaque Value in egglog (not as a
    // vdrmt_variable term), so the egglog rewrite rules cannot fire on it.
    // After the egglog phase, egglog may have created bf2drmt.read ops whose
    // operand is the result of a vdrmt.variable op; the bf2drmt.read verifier
    // rejects this because it only allows bf2drmt.variable (or
    // bf2drmt.struct_extract_ref) as the defining op.
    // Fix: replace every surviving vdrmt.variable with an equivalent
    // bf2drmt.variable.  The result type has already been updated to
    // !bf2drmt.ref<...> by parseValue's setType call, so we use it directly.
    for (mlir::Block &block : funcOp.getFunctionBody().getBlocks()) {
        for (auto it = block.begin(); it != block.end(); ) {
            mlir::Operation &op = *it++;
            if (op.getName().getStringRef() != "vdrmt.variable")
                continue;
            mlir::OpBuilder builder(&op);
            mlir::OperationState state(op.getLoc(), "bf2drmt.variable");
            state.addAttributes(op.getAttrs());
            state.addTypes(op.getResultTypes());
            mlir::Operation *newOp = builder.create(state);
            op.getResult(0).replaceAllUsesWith(newOp->getResult(0));
            op.erase();
        }
    }

    return success();
}

// ============================================================================
// Phase 2: structural conversion helpers
// ============================================================================

/// Collect, in execution order, all bf2drmt.struct_extract_ref and
/// bf2drmt.read ops that transitively define 'val'. Stops at block arguments.
static void collectKeyChain(Value val,
                             llvm::SmallVectorImpl<Operation *> &result) {
    llvm::SmallPtrSet<Operation *, 8> seen;
    llvm::SmallVector<Value> worklist = {val};

    while (!worklist.empty()) {
        Value v = worklist.pop_back_val();
        Operation *defOp = v.getDefiningOp();
        if (!defOp) continue;
        if (!seen.insert(defOp).second) continue;
        if (isa<bf2drmt::StructExtractRefOp>(defOp) ||
            isa<bf2drmt::ReadOp>(defOp)) {
            result.push_back(defOp);
            for (Value operand : defOp->getOperands())
                worklist.push_back(operand);
        }
    }
    // Traversal collected producers last; reverse to get execution order.
    std::reverse(result.begin(), result.end());
}

/// Return the then/else block successors from a vdrmt.if op.
static std::pair<int32_t, int32_t> getIfSuccessors(vdrmt::IfOp ifOp) {
    int32_t thenSucc = -1, elseSucc = -1;
    for (auto &op : ifOp.getThenRegion().front())
        if (auto n = dyn_cast<vdrmt::NextOp>(&op))
            thenSucc = n.getThenSuccessor();
    if (!ifOp.getElseRegion().empty())
        for (auto &op : ifOp.getElseRegion().front())
            if (auto n = dyn_cast<vdrmt::NextOp>(&op))
                elseSucc = n.getThenSuccessor();
    return {thenSucc, elseSucc};
}

/// Recursively convert a vdrmt-chain value to its bf2drmt equivalent.
/// Handles: block args (identity lookup via mapping), vdrmt.struct_extract
/// (converts the read+struct_extract pair to struct_extract_ref+read).
/// Emits bf2drmt ops into 'b', updating 'mapping'.
static Value convertValToBF2DRMT(Value val, IRMapping &mapping, OpBuilder &b) {
    if (mapping.contains(val))
        return mapping.lookup(val);

    Operation *defOp = val.getDefiningOp();
    if (!defOp)
        return mapping.lookupOrDefault(val); // block argument

    if (auto seOp = dyn_cast<vdrmt::StructExtractOp>(defOp)) {
        // Pattern: vdrmt.read(ref) + vdrmt.struct_extract(readResult, idx)
        // → bf2drmt.struct_extract_ref(bf3Ref, idx) + bf2drmt.read(fieldRef)
        Value srcInput = seOp.getInput();
        Value bf3Ref;
        if (auto srcRead = srcInput.getDefiningOp<vdrmt::ReadOp>())
            bf3Ref = convertValToBF2DRMT(srcRead.getRef(), mapping, b);
        else
            bf3Ref = convertValToBF2DRMT(srcInput, mapping, b);

        Type fieldTy     = convertToBF2DRMTType(seOp.getResult().getType());
        Type fieldRefTy  = bf2drmt::ReferenceType::get(b.getContext(), fieldTy);
        auto seRefOp     = b.create<bf2drmt::StructExtractRefOp>(
            seOp.getLoc(), fieldRefTy, bf3Ref,
            b.getI32IntegerAttr(seOp.getFieldIndex()));
        auto readOp      = b.create<bf2drmt::ReadOp>(
            seOp.getLoc(), fieldTy, seRefOp.getResult());
        mapping.map(val, readOp.getResult());
        return readOp.getResult();
    }

    // Fallback: recursively convert operands, then clone the op.
    for (Value operand : defOp->getOperands())
        convertValToBF2DRMT(operand, mapping, b);
    auto *cloned = b.clone(*defOp, mapping);
    for (unsigned i = 0; i < defOp->getNumResults(); ++i)
        mapping.map(defOp->getResult(i), cloned->getResult(i));
    return (defOp->getNumResults() > 0) ? mapping.lookup(val) : Value();
}

/// Convert a single (DialEgg-lowered) func.func into a bf2drmt.pipe.
///
/// After DialEgg, the function body may still contain vdrmt.cmp / vdrmt.if
/// (not lowered by DialEgg — they are structural) plus bf2drmt ops created
/// by DialEgg. Everything else should already be bf2drmt after DialEgg.
static LogicalResult convertFuncToPipe(func::FuncOp funcOp) {
    MLIRContext *ctx = funcOp.getContext();
    Block &funcEntry = funcOp.getFunctionBody().front();

    // ── Check for hash5tuple block first ─────────────────────────────────
    vdrmt::Hash5TupleApplyOp hashApplyOp = nullptr;
    funcOp.walk([&](vdrmt::Hash5TupleApplyOp op) {
        hashApplyOp = op;
        return WalkResult::interrupt();
    });

    if (hashApplyOp) {
        // ── Hash pipe: vdrmt.hash5tuple.apply → bf2drmt.pipe type(hash) ───
        // The hash pipe uses match_mask (not match) for the 5-tuple fields.
        // Each hash bucket selects an entry; the action sets the next block.

        int32_t nrEntries = 64;
        funcOp->getParentOfType<ModuleOp>().walk(
            [&](vdrmt::Hash5TupleInstanceOp inst) {
                if (inst.getSymName() == hashApplyOp.getInstance())
                    nrEntries = (int32_t)inst.getNrEntries();
            });

        OpBuilder builder(funcOp);
        auto pipeOp = builder.create<bf2drmt::PipeOp>(
            funcOp.getLoc(),
            funcOp.getName(),
            bf2drmt::BF2DRMTPipeType::Hash,
            /*match_mask=*/nullptr,
            builder.getI32IntegerAttr(nrEntries),
            /*annotations=*/nullptr);
        Block &pipeEntry = pipeOp.getBody().emplaceBlock();
        IRMapping mapping;
        for (BlockArgument arg : funcEntry.getArguments()) {
            BlockArgument pipeArg =
                pipeEntry.addArgument(convertToBF2DRMTType(arg.getType()), arg.getLoc());
            mapping.map(arg, pipeArg);
        }
        builder.setInsertionPointToStart(&pipeEntry);

        // Build table_key with all 5-tuple fields.
        // DOCA hash pipe: fields in table_key → go into match_mask (hash function selection).
        auto tableKeyOp = builder.create<bf2drmt::TableKeyOp>(funcOp.getLoc());
        Block &tkBlock  = tableKeyOp.getBody().emplaceBlock();
        OpBuilder tkBuilder(ctx);
        tkBuilder.setInsertionPointToStart(&tkBlock);

        auto buildKeyField = [&](Value vdrmtVal) -> Value {
            return convertValToBF2DRMT(vdrmtVal, mapping, tkBuilder);
        };

        // All five tuple fields; only the last MatchKeyOp is the terminator.
        // Emit all five as match_key ops — the codegen will loop over them.
        // Since MatchKeyOp is [Terminator], only ONE can appear per region.
        // We use a helper attribute on the table_key to encode all hash fields.
        // For now, use src_addr as the match_key terminator and annotate the rest.
        Value srcBF3   = buildKeyField(hashApplyOp.getSrcAddr());
        Value dstBF3   = buildKeyField(hashApplyOp.getDstAddr());
        Value sportBF3 = buildKeyField(hashApplyOp.getSrcPort());
        Value dportBF3 = buildKeyField(hashApplyOp.getDstPort());
        Value protoBF3 = buildKeyField(hashApplyOp.getProto());
        // The table_key terminator is match_key on src_addr.
        // The other fields are preserved in bf2drmt.read ops already emitted
        // into the table_key block by buildKeyField — the codegen can walk them.
        tkBuilder.create<bf2drmt::MatchKeyOp>(hashApplyOp.getLoc(), srcBF3);
        (void)dstBF3; (void)sportBF3; (void)dportBF3; (void)protoBF3;

        // Determine the successor for the default action.
        int32_t nextSucc = -1;
        funcOp.walk([&](vdrmt::NextOp nop) {
            nextSucc = nop.getThenSuccessor();
        });

        auto defSym   = mlir::SymbolRefAttr::get(ctx, "_default");
        auto actionOp = builder.create<bf2drmt::PipeActionOp>(
            funcOp.getLoc(), defSym, /*annotations=*/nullptr);
        Block &actionBlock = actionOp.getBody().emplaceBlock();
        OpBuilder actionBuilder(ctx);
        actionBuilder.setInsertionPointToStart(&actionBlock);
        actionBuilder.create<bf2drmt::NextOp>(funcOp.getLoc(), nextSucc);

        funcOp.erase();
        return success();
    }

    // Locate structural vdrmt ops (at function body top level only).
    vdrmt::CmpOp  cmpOp      = nullptr;
    vdrmt::IfOp   ifOp       = nullptr;
    vdrmt::NextOp topNextOp  = nullptr;

    for (auto &op : funcEntry) {
        if (!cmpOp)     cmpOp     = dyn_cast<vdrmt::CmpOp>(&op);
        if (!ifOp)      ifOp      = dyn_cast<vdrmt::IfOp>(&op);
        if (!topNextOp) topNextOp = dyn_cast<vdrmt::NextOp>(&op);
    }

    // Create the bf2drmt.pipe op immediately before the func.
    OpBuilder builder(funcOp);
    auto pipeOp = builder.create<bf2drmt::PipeOp>(
        funcOp.getLoc(),
        funcOp.getName(),
        bf2drmt::BF2DRMTPipeType::Basic,
        /*match_mask=*/nullptr,
        /*nr_entries=*/nullptr,
        /*annotations=*/nullptr);

    // The generated builder does NOT create blocks; add the entry block.
    Block &pipeEntry = pipeOp.getBody().emplaceBlock();

    // Mirror the function's block arguments onto the pipe body.
    IRMapping mapping;
    for (BlockArgument arg : funcEntry.getArguments()) {
        BlockArgument pipeArg =
            pipeEntry.addArgument(convertToBF2DRMTType(arg.getType()), arg.getLoc());
        mapping.map(arg, pipeArg);
    }

    builder.setInsertionPointToStart(&pipeEntry);

    if (cmpOp && ifOp) {
        // ── Table-based routing: cmp → if ────────────────────────────────
        // Collect the bf2drmt read chain that produces the compared value
        // and place it in a table_key region.

        Value keyVal = cmpOp.getLhs();

        llvm::SmallVector<Operation *> keyOps;
        collectKeyChain(keyVal, keyOps);

        // Build table_key and populate its body.
        auto tableKeyOp = builder.create<bf2drmt::TableKeyOp>(funcOp.getLoc());
        Block &tkBlock  = tableKeyOp.getBody().emplaceBlock();
        OpBuilder tkBuilder(ctx);
        tkBuilder.setInsertionPointToStart(&tkBlock);

        IRMapping tkMapping(mapping);
        for (Operation *op : keyOps)
            tkBuilder.clone(*op, tkMapping);

        Value clonedKey = tkMapping.lookupOrDefault(keyVal);
        tkBuilder.create<bf2drmt::MatchKeyOp>(cmpOp.getLoc(), clonedKey);

        // Collect top-level side-effectful ops (meter/counter) that appear before
        // the cmp/if — these run for every packet and must appear in the pipe
        // actions so the codegen can configure the pipe monitor.
        llvm::SmallVector<Operation *> prologueOps;
        for (auto &op : funcEntry) {
            if (&op == cmpOp.getOperation() || &op == ifOp.getOperation()) break;
            if (isa<vdrmt::CounterCountOp, vdrmt::MeterExecuteOp,
                vdrmt::RegisterReadOp,  vdrmt::RegisterWriteOp>(&op))
                prologueOps.push_back(&op);
        }

        // Build pipe_action @_hit { <prologue>; <then-region body>; bf2drmt.next N_then }.
        auto [thenSucc, elseSucc] = getIfSuccessors(ifOp);

        auto hitSym    = mlir::SymbolRefAttr::get(ctx, "_hit");
        auto hitAction = builder.create<bf2drmt::PipeActionOp>(
            funcOp.getLoc(), hitSym, /*annotations=*/nullptr);
        Block &hitBlock = hitAction.getBody().emplaceBlock();
        OpBuilder hitBuilder(ctx);
        hitBuilder.setInsertionPointToStart(&hitBlock);
        IRMapping hitMapping(mapping);

        // Emit prologue side-effectful ops (meter/counter) first.
        auto emitSideEffectOp = [&](Operation &op, OpBuilder &b, IRMapping &m) {
            if (auto counterOp = dyn_cast<vdrmt::CounterCountOp>(&op)) {
                Value bf3Idx = convertValToBF2DRMT(counterOp.getIndex(), m, b);
                b.create<vdrmt::CounterCountOp>(
                    counterOp.getLoc(), counterOp.getInstance(), bf3Idx);
                return;
            }
            if (auto meterOp = dyn_cast<vdrmt::MeterExecuteOp>(&op)) {
                Value bf3Idx = convertValToBF2DRMT(meterOp.getIndex(), m, b);
                auto newOp = b.create<vdrmt::MeterExecuteOp>(
                    meterOp.getLoc(), meterOp.getColor().getType(),
                    meterOp.getInstance(), bf3Idx);
                m.map(meterOp.getColor(), newOp.getColor());
            }
            if (auto regRead = dyn_cast<vdrmt::RegisterReadOp>(&op)) {
                Value bf3Idx = convertValToBF2DRMT(regRead.getIndex(), m, b);
                auto newOp = b.create<vdrmt::RegisterReadOp>(
                    regRead.getLoc(), regRead.getResult().getType(),
                    regRead.getInstance(), bf3Idx);
                m.map(regRead.getResult(), newOp.getResult());
            }
            if (auto regWrite = dyn_cast<vdrmt::RegisterWriteOp>(&op)) {
                Value bf3Idx = convertValToBF2DRMT(regWrite.getIndex(), m, b);
                Value bf3Val = convertValToBF2DRMT(regWrite.getValue(), m, b);
                b.create<vdrmt::RegisterWriteOp>(
                    regWrite.getLoc(), regWrite.getInstance(), bf3Idx, bf3Val);
            }
        };
        for (Operation *op : prologueOps)
            emitSideEffectOp(*op, hitBuilder, hitMapping);

        // Emit ops from the then-region.
        for (auto &op : ifOp.getThenRegion().front()) {
            if (isa<vdrmt::CounterCountOp, vdrmt::MeterExecuteOp,
                    vdrmt::RegisterReadOp,  vdrmt::RegisterWriteOp>(&op)) {
                emitSideEffectOp(op, hitBuilder, hitMapping);
                continue;
            }
            // Skip other vdrmt dialect ops (structural or dead).
            if (op.getDialect()->getNamespace() == "vdrmt") continue;
            // Non-vdrmt ops (e.g. bf2drmt ops from DialEgg): clone as-is.
            hitBuilder.clone(op, hitMapping);
        }
        hitBuilder.create<bf2drmt::NextOp>(funcOp.getLoc(), thenSucc);

        // Build pipe_action @_miss { <prologue>; bf2drmt.next N_else }.
        auto missSym    = mlir::SymbolRefAttr::get(ctx, "_miss");
        auto missAction = builder.create<bf2drmt::PipeActionOp>(
            funcOp.getLoc(), missSym, /*annotations=*/nullptr);
        Block &missBlock = missAction.getBody().emplaceBlock();
        OpBuilder missBuilder(ctx);
        missBuilder.setInsertionPointToStart(&missBlock);
        IRMapping missMapping(mapping);
        // Prologue side-effectful ops run in _miss too (they run for every packet).
        for (Operation *op : prologueOps)
            emitSideEffectOp(*op, missBuilder, missMapping);
        missBuilder.create<bf2drmt::NextOp>(funcOp.getLoc(), elseSucc);

    } else {
        // ── Action-only: collect all non-structural ops ───────────────────
        int32_t nextSucc = -1;
        llvm::SmallVector<Operation *> opsToClone;

        for (auto &op : funcEntry) {
            if (isa<func::ReturnOp>(&op)) continue;
            if (auto n = dyn_cast<vdrmt::NextOp>(&op)) {
                nextSucc = n.getThenSuccessor();
                continue;
            }
            opsToClone.push_back(&op);
        }

        auto defSym   = mlir::SymbolRefAttr::get(ctx, "_default");
        auto actionOp = builder.create<bf2drmt::PipeActionOp>(
            funcOp.getLoc(), defSym, /*annotations=*/nullptr);
        Block &actionBlock = actionOp.getBody().emplaceBlock();
        OpBuilder actionBuilder(ctx);
        actionBuilder.setInsertionPointToStart(&actionBlock);

        for (Operation *op : opsToClone)
            actionBuilder.clone(*op, mapping);

        actionBuilder.create<bf2drmt::NextOp>(funcOp.getLoc(), nextSucc);
    }

    funcOp.erase();
    return success();
}

/// Replace every func.func in 'mod' with a bf2drmt.pipe.
static LogicalResult restructureToBF2DRMTPipes(ModuleOp mod) {
    llvm::SmallVector<func::FuncOp> funcs(mod.getOps<func::FuncOp>());
    for (auto funcOp : funcs)
        if (failed(convertFuncToPipe(funcOp)))
            return failure();
    return success();
}

// ============================================================================
// Phase 3: Mapper egglog emitter
// ============================================================================

/// Serialize a bf2drmt (or vdrmt / builtin integer) type to its mapper egglog form.
static std::string typeToMapperEgglog(mlir::Type type) {
    if (auto intTy = mlir::dyn_cast<mlir::IntegerType>(type)) {
        switch (intTy.getWidth()) {
            case 1:  return "(I1)";
            case 8:  return "(I8)";
            case 9:  return "(I9)";
            case 16: return "(I16)";
            case 32: return "(I32)";
            case 64: return "(I64)";
            default: return "(Int " + std::to_string(intTy.getWidth()) + ")";
        }
    }
    if (auto refTy = mlir::dyn_cast<bf2drmt::ReferenceType>(type))
        return "(bf2drmt_ref " + typeToMapperEgglog(refTy.getObjectType()) + ")";
    if (mlir::isa<bf2drmt::StructType>(type)) {
        std::string s; llvm::raw_string_ostream os(s); type.print(os);
        return "(bf2drmt_struct \"" + escapeEgglogString(s) + "\")";
    }
    if (mlir::isa<bf2drmt::HeaderType>(type)) {
        std::string s; llvm::raw_string_ostream os(s); type.print(os);
        return "(bf2drmt_header \"" + escapeEgglogString(s) + "\")";
    }
    // Fallback: print as opaque
    std::string s; llvm::raw_string_ostream os(s); type.print(os);
    return "(OtherType \"" + escapeEgglogString(s) + "\")";
}

/// Serialize an attribute to mapper egglog form.
static std::string attrToMapperEgglog(mlir::Attribute attr) {
    if (auto ia = mlir::dyn_cast<mlir::IntegerAttr>(attr))
        return "(IntegerAttr " + std::to_string(ia.getInt())
               + " " + typeToMapperEgglog(ia.getType()) + ")";
    if (auto sa = mlir::dyn_cast<mlir::StringAttr>(attr))
        return "(StringAttr \"" + escapeEgglogString(sa.getValue().str()) + "\")";
    std::string s; llvm::raw_string_ostream os(s); attr.print(os);
    return "(OpaqueAttr \"" + escapeEgglogString(s) + "\")";
}

/// Convert a single bf2drmt op to its mapper egglog expression (no `let` prefix).
/// Returns "" for structural ops (pipe, pipe_action, table_key, next) that are
/// not included in the op cost model.
static std::string bf2drmtOpToMapperEgglog(
    mlir::Operation &op,
    const llvm::DenseMap<mlir::Value, std::string> &names)
{
    // Look up SSA value — block args are stored inline as "(Value N <type>)".
    auto val = [&](mlir::Value v) -> std::string {
        auto it = names.find(v);
        return (it != names.end()) ? it->second : "(OpaqueValue)";
    };

    if (auto serOp = mlir::dyn_cast<bf2drmt::StructExtractRefOp>(&op)) {
        return "(bf2drmt_struct_extract_ref "
               + val(serOp.getInput())
               + " (NamedAttr \"fieldIndex\" (IntegerAttr "
               + std::to_string(serOp.getFieldIndex()) + " (I32))) "
               + typeToMapperEgglog(serOp.getResult().getType()) + ")";
    }
    if (auto readOp = mlir::dyn_cast<bf2drmt::ReadOp>(&op)) {
        return "(bf2drmt_read "
               + val(readOp.getRef())
               + " " + typeToMapperEgglog(readOp.getResult().getType()) + ")";
    }
    if (auto constOp = mlir::dyn_cast<bf2drmt::ConstantOp>(&op)) {
        return "(bf2drmt_constant (NamedAttr \"value\" "
               + attrToMapperEgglog(constOp.getValue()) + ") "
               + typeToMapperEgglog(constOp.getResult().getType()) + ")";
    }
    if (auto binOp = mlir::dyn_cast<bf2drmt::BinOp>(&op)) {
        return "(bf2drmt_binop "
               + val(binOp.getLhs()) + " " + val(binOp.getRhs())
               + " (NamedAttr \"kind\" (IntegerAttr "
               + std::to_string(static_cast<int>(binOp.getKind())) + " (I32))) "
               + typeToMapperEgglog(binOp.getResult().getType()) + ")";
    }
    if (auto assignOp = mlir::dyn_cast<bf2drmt::AssignOp>(&op)) {
        return "(bf2drmt_assign "
               + val(assignOp.getValue()) + " "
               + val(assignOp.getRef()) + ")";
    }
    if (auto matchOp = mlir::dyn_cast<bf2drmt::MatchKeyOp>(&op)) {
        return "(bf2drmt_match_key "
               + val(matchOp.getKey()) + ")";
    }
    if (auto castOp = mlir::dyn_cast<bf2drmt::CastOp>(&op)) {
        return "(bf2drmt_cast "
               + val(castOp.getValue())
               + " " + typeToMapperEgglog(castOp.getResult().getType()) + ")";
    }
    if (auto shlOp = mlir::dyn_cast<bf2drmt::ShlOp>(&op)) {
        return "(bf2drmt_shl "
               + val(shlOp.getLhs()) + " " + val(shlOp.getRhs())
               + " " + typeToMapperEgglog(shlOp.getResult().getType()) + ")";
    }
    if (auto shrOp = mlir::dyn_cast<bf2drmt::ShrOp>(&op)) {
        return "(bf2drmt_shr "
               + val(shrOp.getLhs()) + " " + val(shrOp.getRhs())
               + " " + typeToMapperEgglog(shrOp.getResult().getType()) + ")";
    }
    if (auto sliceOp = mlir::dyn_cast<bf2drmt::SliceOp>(&op)) {
        return "(bf2drmt_slice "
               + val(sliceOp.getInput())
               + " (NamedAttr \"hi\" (IntegerAttr "
               + std::to_string(sliceOp.getHighBit()) + " (I32)))"
               + " (NamedAttr \"lo\" (IntegerAttr "
               + std::to_string(sliceOp.getLowBit()) + " (I32)))"
               + " " + typeToMapperEgglog(sliceOp.getResult().getType()) + ")";
    }
    if (auto concatOp = mlir::dyn_cast<bf2drmt::ConcatOp>(&op)) {
        return "(bf2drmt_concat "
               + val(concatOp.getUpper()) + " " + val(concatOp.getLower())
               + " " + typeToMapperEgglog(concatOp.getResult().getType()) + ")";
    }
    // Structural ops — skip
    return "";
}

/// Per-block data collected for the mapper.
struct MapperBlockInfo {
    int blockId = -1;
    std::vector<int> successors;
    // (let-name, egglog-expr) pairs in order
    std::vector<std::pair<std::string, std::string>> ops;
};

/// Walk all regions of a bf2drmt.pipe and collect ops for the mapper.
static MapperBlockInfo eggifyPipeForMapper(bf2drmt::PipeOp pipeOp) {
    MapperBlockInfo info;

    // Extract numeric block ID from "@vdrmt_blockN"
    std::string symName = pipeOp.getSymName().str();
    std::string suffix;
    for (char c : symName)
        if (std::isdigit(c)) suffix += c;
    info.blockId = suffix.empty() ? -1 : std::stoi(suffix);

    std::string prefix = "blk" + std::to_string(info.blockId) + "_drmt";
    int opIdx = 0;
    llvm::DenseMap<mlir::Value, std::string> names;

    // Register block entry args as inline Value expressions.
    auto &entryBlock = pipeOp.getBody().front();
    for (auto [i, arg] : llvm::enumerate(entryBlock.getArguments()))
        names[arg] = "(Value " + std::to_string(i)
                     + " " + typeToMapperEgglog(arg.getType()) + ")";

    // Walk every op nested in the pipe (including table_key / pipe_action bodies).
    pipeOp->walk([&](mlir::Operation *op) {
        // Skip container ops themselves — we descend into their bodies.
        if (mlir::isa<bf2drmt::PipeOp, bf2drmt::PipeActionOp,
                      bf2drmt::TableKeyOp>(op))
            return mlir::WalkResult::advance();

        // Collect successors from bf2drmt.next.
        if (auto nextOp = mlir::dyn_cast<bf2drmt::NextOp>(op)) {
            info.successors.push_back(nextOp.getSuccessor());
            return mlir::WalkResult::advance();
        }

        std::string expr = bf2drmtOpToMapperEgglog(*op, names);
        if (expr.empty())
            return mlir::WalkResult::advance();

        std::string opName = prefix + "_op" + std::to_string(opIdx++);
        // Register result(s) so subsequent ops can reference them.
        for (mlir::Value res : op->getResults())
            names[res] = opName;

        info.ops.emplace_back(opName, expr);
        return mlir::WalkResult::advance();
    });

    return info;
}

// ============================================================================
// vFFS → bf2drmt lowering helper
// ============================================================================

/// Replace vdrmt.counter_instance → bf2drmt.counter_decl and
/// vdrmt.counter.count → bf2drmt.counter.count within a module.
static void lowerVFFSCounters(ModuleOp mod, OpBuilder &builder) {
    // 1. Replace vdrmt.counter_instance → bf2drmt.counter_decl
    SmallVector<vdrmt::CounterInstanceOp> instances;
    mod.walk([&](vdrmt::CounterInstanceOp op) { instances.push_back(op); });
    for (auto inst : instances) {
        builder.setInsertionPoint(inst);
        auto nameAttr = builder.getStringAttr(inst.getSymName());
        builder.create<bf2drmt::CounterDeclOp>(
            inst.getLoc(), nameAttr, inst.getSizeAttr());
        inst.erase();
    }

    // 2. Replace vdrmt.counter.count → bf2drmt.counter.count
    SmallVector<vdrmt::CounterCountOp> counts;
    mod.walk([&](vdrmt::CounterCountOp op) { counts.push_back(op); });
    for (auto count : counts) {
        builder.setInsertionPoint(count);
        builder.create<bf2drmt::CounterCountOp>(
            count.getLoc(), count.getInstance(), count.getIndex());
        count.erase();
    }
}

/// Replace vdrmt.hash5tuple_instance and vdrmt.hash5tuple.apply within a module.
/// hash5tuple_instance is simply removed (the hash pipe decl is implicit in
/// bf2drmt.pipe type(hash) + nr_entries).
/// hash5tuple.apply ops are erased (their work is done in convertFuncToPipe).
static void lowerVFFSHash5Tuple(ModuleOp mod) {
    SmallVector<vdrmt::Hash5TupleInstanceOp> instances;
    mod.walk([&](vdrmt::Hash5TupleInstanceOp op) { instances.push_back(op); });
    for (auto inst : instances)
        inst.erase();

    SmallVector<vdrmt::Hash5TupleApplyOp> applies;
    mod.walk([&](vdrmt::Hash5TupleApplyOp op) { applies.push_back(op); });
    for (auto apply : applies)
        apply.erase();
}

/// Replace vdrmt.meter_instance → bf2drmt.meter_decl and
/// vdrmt.meter.execute → bf2drmt.meter.execute within a module.
static void lowerVDRMTMeters(ModuleOp mod, OpBuilder &builder) {
    // 1. Replace vdrmt.meter_instance → bf2drmt.meter_decl
    SmallVector<vdrmt::MeterInstanceOp> instances;
    mod.walk([&](vdrmt::MeterInstanceOp op) { instances.push_back(op); });
    for (auto inst : instances) {
        builder.setInsertionPoint(inst);
        auto nameAttr = builder.getStringAttr(inst.getSymName());
        builder.create<bf2drmt::MeterDeclOp>(
            inst.getLoc(), nameAttr, inst.getSizeAttr(), inst.getMeterTypeAttr());
        inst.erase();
    }

    // 2. Replace vdrmt.meter.execute → bf2drmt.meter.execute
    SmallVector<vdrmt::MeterExecuteOp> execs;
    mod.walk([&](vdrmt::MeterExecuteOp op) { execs.push_back(op); });
    for (auto exec : execs) {
        builder.setInsertionPoint(exec);
        auto newOp = builder.create<bf2drmt::MeterExecuteOp>(
            exec.getLoc(), exec.getColor().getType(),
            exec.getInstance(), exec.getIndex());
        exec.getColor().replaceAllUsesWith(newOp.getColor());
        exec.erase();
    }
}

/// BF2 has no general-purpose P4 register primitive in DOCA Flow 2.2.
/// Pattern A (counter-inc → doca_flow_shared_counter) lands in Stage 2.
/// Pattern B (entry-as-register → ARM-driven hash-pipe entry update) lands
/// in Stage 3. Until then, reject any vdrmt.register.* usage so we don't
/// silently emit invalid DOCA Flow code.
static LogicalResult lowerVDRMTRegisters(ModuleOp mod, OpBuilder &builder) {
    Operation *offender = nullptr;
    mod.walk([&](Operation *op) {
        if (offender) return;
        if (isa<vdrmt::RegisterInstanceOp, vdrmt::RegisterReadOp,
                vdrmt::RegisterWriteOp>(op))
            offender = op;
    });
    if (offender) {
        offender->emitError(
            "BF2DRMT: vdrmt.register.* lowering not yet supported "
            "(Pattern A → doca_flow_shared_counter in Stage 2; "
            "Pattern B → ARM-managed entry-as-register in Stage 3). "
            "DOCA Flow 2.2 has no general-purpose P4 register primitive.");
        return failure();
    }
    return success();
}

// ============================================================================
// BF2DRMT Block Generator
// ============================================================================

class BF2DRMTBlockGenerator {
public:
    BF2DRMTBlockGenerator(MLIRContext *context, StringRef inputDir)
        : context(context), inputDir(inputDir.str()) {}

    LogicalResult generate() {
        int successCount = 0, skipCount = 0;

        // ── Phase 0: cross-block memory access isolation analysis ─────────
        llvm::outs() << "\n  Memory access isolation analysis ...\n";
        runMemoryAnalysis();

        std::error_code ec;
        for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
             dir != end && !ec; dir.increment(ec)) {

            auto path = dir->path();
            if (!llvm::sys::fs::is_directory(path)) continue;

            std::string dirName = llvm::sys::path::filename(path).str();
            if (dirName.find("block") != 0) continue;

            std::string vdrmtFile   = path + "/vdrmt.mlir";
            std::string bf2drmtFile = path + "/bf2drmt.mlir";

            if (!llvm::sys::fs::exists(vdrmtFile)) {
                llvm::outs() << "  ⊘ Skipping " << dirName << " (no vdrmt.mlir)\n";
                skipCount++;
                continue;
            }

            if (failed(generateBlockBF2DRMT(vdrmtFile, bf2drmtFile))) {
                llvm::errs() << "  ✗ Failed to generate "
                             << dirName << "/bf2drmt.mlir\n";
                return failure();
            }
            successCount++;
            llvm::outs() << "  ✓ Generated " << dirName << "/bf2drmt.mlir\n";
        }

        llvm::outs() << "\n  Summary:\n"
                     << "    ✓ Generated: " << successCount << " blocks\n";
        if (skipCount > 0)
            llvm::outs() << "    ⊘ Skipped:   " << skipCount << " blocks\n";

        // ── Phase 3: emit mapper.egg ─────────────────────────────────────
        llvm::outs() << "\n  Generating mapper.egg ...\n";
        if (failed(generateMapperEgglog()))
            return failure();
        llvm::outs() << "  ✓ Generated mapper.egg\n";

        // ── Phase 4: emit doca_flow_pipeline.c ───────────────────────────
        llvm::outs() << "  Generating doca_flow_pipeline.c ...\n";
        if (failed(generateDocaFlowCodeBF2()))
            return failure();
        llvm::outs() << "  ✓ Generated doca_flow_pipeline.c\n";

        return success();
    }

private:
    MLIRContext *context;
    std::string inputDir;

    // =========================================================================
    // Memory Access Isolation Analysis
    // =========================================================================
    //
    // Implements §Memory accessing isolation from the paper:
    //
    //   For each VFFA instance (counter, register, meter, hash5tuple), determine:
    //     • which blocks access it  (locality: local vs. shared)
    //     • whether its index is derived from a packet hash  (partitionable?)
    //
    //   Classification:
    //     Local                  — accessed by exactly one block
    //     SharedPartitionable    — multiple blocks, index ← hash5tuple result
    //     SharedNonPartitionable — multiple blocks, index not hash-derived
    //
    //   Results are written to:
    //     <outputDir>/memory_analysis.json   (module-level summary)
    //     <blockN>/metadata.json             (per-block "memoryAnalysis" section)
    // =========================================================================

    enum class MemoryLocality { Local, SharedPartitionable, SharedNonPartitionable };

    static StringRef localityStr(MemoryLocality l) {
        switch (l) {
        case MemoryLocality::Local:                  return "local";
        case MemoryLocality::SharedPartitionable:    return "shared_partitionable";
        case MemoryLocality::SharedNonPartitionable: return "shared_non_partitionable";
        }
        return "unknown";
    }

    struct VFFAInstanceAnalysis {
        std::string name;
        std::string kind;                    // "counter","register","meter","hash5tuple"
        std::vector<int> accessingBlocks;
        bool allInputsPacketDerived = true;  // false if any input traces to a stateful read
        std::string shardingKey;             // canonical fingerprint of the index expr
        bool shardingKeyConsistent = true;   // false if blocks use different index exprs
        MemoryLocality locality = MemoryLocality::Local;
    };

    /// Walk the SSA def-chain of 'v' and return false if any input is derived
    /// from a stateful read (RegisterReadOp or MeterExecuteOp).
    ///
    /// Rationale: if a stateful op's inputs (index or write value) are purely
    /// packet-derived, the hardware can insert packet steering rules to shard
    /// the stateful memory across ports/engines. If any input traces to another
    /// stateful read the routing decision would itself depend on state, making
    /// sharding impossible.
    ///
    /// Block arguments are conservatively treated as packet-derived: at block
    /// boundaries, values come from hdr/meta which are populated from the packet
    /// before any stateful reads execute in that block.
    static bool isPacketDerived(Value v) {
        SmallVector<Value> worklist = {v};
        DenseSet<Value> visited;
        while (!worklist.empty()) {
            Value cur = worklist.pop_back_val();
            if (!visited.insert(cur).second) continue;
            Operation *def = cur.getDefiningOp();
            if (!def) continue;  // block argument — conservatively packet-derived
            // A value produced by a stateful read is state-derived, not packet-derived.
            if (isa<RegisterReadOp, MeterExecuteOp>(def)) return false;
            // Trace through all other ops (arithmetic, casts, hash, struct access, etc.).
            for (Value operand : def->getOperands())
                worklist.push_back(operand);
        }
        return true;
    }

    /// Compute a canonical fingerprint string for the def-chain of value 'v'.
    ///
    /// Two values with identical fingerprints use the same structural path
    /// through hdr/meta fields, meaning they will select the same hardware
    /// shard for any given packet — so bf2drmt per-entry mode can safely
    /// partition the resource using this key.
    ///
    /// Conventions:
    ///   "arg:N"                   — block argument (hdr/meta ref)
    ///   "read(fp)"                — vdrmt.read from a reference
    ///   "struct_extract[N](fp)"   — field extraction at index N
    ///   "hash5tuple(fp0,...,fp4)" — 5-tuple hash (all input fields)
    ///   "const"                   — any compile-time constant
    ///   "op_name(fp0,...)"        — generic fallback for arithmetic, casts, etc.
    static std::string keyFingerprint(Value v, DenseSet<Value> &visited) {
        if (!visited.insert(v).second)
            return "cycle"; // SSA is a DAG; shouldn't fire in practice

        // Block argument — hdr or meta reference, packet-derived by convention.
        if (auto ba = dyn_cast<BlockArgument>(v))
            return "arg:" + std::to_string(ba.getArgNumber());

        Operation *def = v.getDefiningOp();
        if (!def) return "unknown";

        // vdrmt.constant — compile-time constant
        if (isa<ConstantOp>(def))
            return "const";

        // vdrmt.read %ref — load from a reference (hdr/meta field pointer)
        if (isa<ReadOp>(def))
            return "read(" + keyFingerprint(def->getOperand(0), visited) + ")";

        // vdrmt.struct_extract %input[idx] — struct field selection by index
        if (auto extractOp = dyn_cast<StructExtractOp>(def))
            return "struct_extract[" + std::to_string(extractOp.getFieldIndex()) + "]("
                   + keyFingerprint(extractOp.getInput(), visited) + ")";

        // vdrmt.hash5tuple.apply @inst(src,dst,sport,dport,proto)
        // The 5-tuple is the key; encode all operand fingerprints.
        if (isa<Hash5TupleApplyOp>(def)) {
            std::string fp = "hash5tuple(";
            bool first = true;
            for (Value operand : def->getOperands()) {
                if (!first) fp += ",";
                fp += keyFingerprint(operand, visited);
                first = false;
            }
            return fp + ")";
        }

        // Generic fallback: op mnemonic + operand fingerprints.
        // Handles arithmetic (add, mul, shift), casts, etc.
        std::string fp = def->getName().getStringRef().str() + "(";
        bool first = true;
        for (Value operand : def->getOperands()) {
            if (!first) fp += ",";
            fp += keyFingerprint(operand, visited);
            first = false;
        }
        return fp + ")";
    }

    /// Return the sharding key fingerprint for a VFFA execute op.
    ///
    /// For register/counter/meter — the index (first SSA operand) determines
    /// which hardware shard the packet maps to.
    /// For hash5tuple — all 5 tuple operands together form the key.
    static std::string shardingKeyOf(VFFAExecuteOpInterface use) {
        Operation *op = use.getOperation();
        auto operands = op->getOperands();
        if (operands.empty()) return "";

        if (isa<Hash5TupleApplyOp>(op)) {
            // All 5 inputs jointly form the sharding key.
            std::string key = "hash5tuple(";
            bool first = true;
            for (Value operand : operands) {
                if (!first) key += ",";
                DenseSet<Value> visited;
                key += keyFingerprint(operand, visited);
                first = false;
            }
            return key + ")";
        }

        // register.read/write, counter.count, meter.execute:
        // first operand is the index.
        DenseSet<Value> visited;
        return keyFingerprint(operands[0], visited);
    }

    /// Determine the VFFA kind string from an execute op.
    static std::string vffaKindOf(VFFAExecuteOpInterface use) {
        Operation *op = use.getOperation();
        if (isa<CounterCountOp>(op))      return "counter";
        if (isa<MeterExecuteOp>(op))      return "meter";
        if (isa<Hash5TupleApplyOp>(op))   return "hash5tuple";
        if (isa<RegisterReadOp>(op) ||
            isa<RegisterWriteOp>(op))     return "register";
        return "unknown";
    }

    void runMemoryAnalysis() {
        // ── Pass 1: scan every vdrmt.mlir and collect per-instance access info ──
        //
        // instanceMap : name → VFFAInstanceAnalysis (accumulated across blocks)
        // blockInstances : blockId → list of instance names accessed
        llvm::StringMap<VFFAInstanceAnalysis> instanceMap;
        std::map<int, std::vector<std::string>> blockInstances;

        // Collect block directories and sort by block ID before processing so
        // that accessingBlocks order and the first-seen shardingKey are
        // deterministic across runs (directory_iterator order is filesystem-
        // dependent and varies between ext4 runs).
        std::vector<std::pair<int, std::string>> blockPaths; // (blockId, path)
        {
            std::error_code ec;
            for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
                 dir != end && !ec; dir.increment(ec)) {
                auto path = dir->path();
                if (!llvm::sys::fs::is_directory(path)) continue;
                std::string dirName = llvm::sys::path::filename(path).str();
                if (dirName.find("block") != 0) continue;
                std::string vdrmtFile = path + "/vdrmt.mlir";
                if (!llvm::sys::fs::exists(vdrmtFile)) continue;
                // Parse just to extract block_id, then keep for main loop.
                auto mod = parseSourceFile<ModuleOp>(vdrmtFile, context);
                if (!mod) continue;
                int blockId = -1;
                mod->walk([&](func::FuncOp fn) {
                    if (auto attr = fn->getAttrOfType<IntegerAttr>("vdrmt.block_id"))
                        blockId = (int)attr.getInt();
                    return WalkResult::interrupt();
                });
                if (blockId >= 0)
                    blockPaths.emplace_back(blockId, path);
            }
            llvm::sort(blockPaths, [](auto &a, auto &b) { return a.first < b.first; });
        }

        for (auto &[blockId, path] : blockPaths) {
            std::string vdrmtFile = path + "/vdrmt.mlir";
            auto mod = parseSourceFile<ModuleOp>(vdrmtFile, context);
            if (!mod) continue;

            // Walk all VFFA execute ops via the interface.
            mod->walk([&](VFFAExecuteOpInterface use) {
                std::string instName = use.getInstanceAttr().getValue().str();
                std::string kind     = vffaKindOf(use);

                // Check that ALL inputs (index and write value) are packet-derived.
                // If any input traces back to a stateful read, sharding via packet
                // steering is impossible for this instance.
                //
                // Use getOperation()->getOperands() rather than getInputs(): every
                // getInputs() impl returns ValueRange{getSrcAddr(), ...} which is
                // backed by a temporary initializer_list destroyed on return, so
                // the ValueRange would dangle by the time we iterate it.
                // getOperands() is always safe — it's backed by the op's own storage.
                // ($instance is a FlatSymbolRefAttr, not an SSA operand, so it does
                // not appear in getOperands().)
                bool allPacket = true;
                for (Value input : use.getOperation()->getOperands())
                    if (!isPacketDerived(input)) { allPacket = false; break; }

                // Compute the sharding key fingerprint for this access.
                std::string thisKey = shardingKeyOf(use);

                // Accumulate into the module-level map.
                auto &entry = instanceMap[instName];
                if (entry.name.empty()) {
                    entry.name = instName;
                    entry.kind = kind;
                }
                // Only record each blockId once per instance.
                if (llvm::find(entry.accessingBlocks, blockId) ==
                    entry.accessingBlocks.end()) {
                    entry.accessingBlocks.push_back(blockId);
                }
                // One non-packet-derived access taints the whole instance.
                entry.allInputsPacketDerived =
                    entry.allInputsPacketDerived && allPacket;
                // Track sharding key consistency across blocks:
                // all accesses must use the same index expression so that
                // bf2drmt per-entry mode can shard by a single packet key.
                if (entry.shardingKey.empty()) {
                    entry.shardingKey = thisKey;
                } else if (entry.shardingKey != thisKey) {
                    entry.shardingKeyConsistent = false;
                }

                // Record this instance in the per-block list.
                auto &blkList = blockInstances[blockId];
                if (llvm::find(blkList, instName) == blkList.end())
                    blkList.push_back(instName);
            });
        }

        // Sort accessingBlocks within each instance for deterministic JSON output.
        for (auto &kv : instanceMap)
            llvm::sort(kv.second.accessingBlocks);

        // ── Pass 2: classify each instance ────────────────────────────────────
        // SharedPartitionable (maps to bf2drmt per-entry mode) requires:
        //   • accessed by multiple blocks, AND
        //   • all inputs are packet-derived (no stateful-read dependencies), AND
        //   • all blocks use the same index expression (consistent sharding key)
        //     so that a single packet steering rule selects the correct shard.
        //
        // If inputs are packet-derived but keys differ across blocks, bf2drmt
        // per-entry mode cannot be used (different blocks would select different
        // shards for the same packet). Shared mode (counter, meter) can still work.
        for (auto &kv : instanceMap) {
            auto &info = kv.second;
            if (info.accessingBlocks.size() == 1) {
                info.locality = MemoryLocality::Local;
            } else if (info.allInputsPacketDerived && info.shardingKeyConsistent) {
                info.locality = MemoryLocality::SharedPartitionable;
            } else {
                info.locality = MemoryLocality::SharedNonPartitionable;
            }
        }

        // ── Pass 3: write module-level memory_analysis.json ───────────────────
        // Collect sorted instance names for deterministic JSON output
        // (llvm::StringMap iteration order is unspecified).
        std::vector<std::string> sortedInstNames;
        for (auto &kv : instanceMap)
            sortedInstNames.push_back(kv.first().str());
        llvm::sort(sortedInstNames);

        {
            std::string outFile = inputDir + "/memory_analysis.json";
            std::error_code errc;
            llvm::raw_fd_ostream out(outFile, errc);
            if (!errc) {
                out << "{\n  \"vffaInstances\": [\n";
                bool firstInst = true;
                for (auto &instName : sortedInstNames) {
                    auto &kv = *instanceMap.find(instName);
                    auto &info = kv.second;
                    if (!firstInst) out << ",\n";
                    firstInst = false;
                    out << "    {\n";
                    out << "      \"name\": \"" << info.name << "\",\n";
                    out << "      \"kind\": \"" << info.kind << "\",\n";
                    out << "      \"locality\": \""
                        << localityStr(info.locality) << "\",\n";
                    out << "      \"allInputsPacketDerived\": "
                        << (info.allInputsPacketDerived ? "true" : "false") << ",\n";
                    out << "      \"shardingKey\": \"" << info.shardingKey << "\",\n";
                    out << "      \"shardingKeyConsistent\": "
                        << (info.shardingKeyConsistent ? "true" : "false") << ",\n";
                    out << "      \"accessingBlocks\": [";
                    for (size_t i = 0; i < info.accessingBlocks.size(); i++) {
                        if (i) out << ", ";
                        out << info.accessingBlocks[i];
                    }
                    out << "]\n    }";
                }
                out << "\n  ]\n}\n";
                llvm::outs() << "  ✓ memory_analysis.json\n";
            }
        }

        // ── Pass 4: annotate each block's metadata.json ────────────────────────
        for (auto &[blockId, instNames] : blockInstances) {
            // Find the block directory.
            std::string blockDir = inputDir + "/block" + std::to_string(blockId);
            std::string metaFile = blockDir + "/metadata.json";
            if (!llvm::sys::fs::exists(metaFile)) continue;

            // Read existing metadata.json.
            auto buf = llvm::MemoryBuffer::getFile(metaFile);
            if (!buf) continue;
            std::string existing = buf.get()->getBuffer().str();

            // Build the memoryAnalysis JSON fragment.
            std::string fragment;
            llvm::raw_string_ostream frag(fragment);
            frag << "  \"memoryAnalysis\": {\n";
            frag << "    \"accessedInstances\": [\n";
            bool firstInst = true;
            for (auto &instName : instNames) {
                auto it = instanceMap.find(instName);
                if (it == instanceMap.end()) continue;
                auto &info = it->second;
                if (!firstInst) frag << ",\n";
                firstInst = false;
                frag << "      {\n";
                frag << "        \"name\": \"" << info.name << "\",\n";
                frag << "        \"kind\": \"" << info.kind << "\",\n";
                frag << "        \"locality\": \""
                     << localityStr(info.locality) << "\",\n";
                frag << "        \"allInputsPacketDerived\": "
                     << (info.allInputsPacketDerived ? "true" : "false") << ",\n";
                frag << "        \"shardingKey\": \"" << info.shardingKey << "\",\n";
                frag << "        \"shardingKeyConsistent\": "
                     << (info.shardingKeyConsistent ? "true" : "false") << "\n";
                frag << "      }";
            }
            frag << "\n    ]\n  }";
            frag.flush();

            // Inject the fragment before the closing '}'.
            // Find the last '}' and insert before it.
            auto pos = existing.rfind('}');
            if (pos == std::string::npos) continue;
            std::string updated = existing.substr(0, pos)
                                + ",\n" + fragment + "\n}";

            std::error_code errc;
            llvm::raw_fd_ostream out(metaFile, errc);
            if (!errc) out << updated;
        }

        // ── Summary ────────────────────────────────────────────────────────────
        int nLocal = 0, nSharedPart = 0, nSharedNonPart = 0;
        for (auto &kv : instanceMap) {
            switch (kv.second.locality) {
            case MemoryLocality::Local:                  nLocal++;        break;
            case MemoryLocality::SharedPartitionable:    nSharedPart++;   break;
            case MemoryLocality::SharedNonPartitionable: nSharedNonPart++;break;
            }
        }
        llvm::outs() << "  Instances: " << instanceMap.size()
                     << " total  ("
                     << nLocal        << " local, "
                     << nSharedPart   << " shared-partitionable, "
                     << nSharedNonPart<< " shared-non-partitionable)\n";
    }

    LogicalResult generateMapperEgglog() {
        // ── Collect per-block info from all generated bf2drmt.mlir files ──
        std::vector<MapperBlockInfo> blockInfos;

        std::error_code ec;
        for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
             dir != end && !ec; dir.increment(ec)) {

            auto path = dir->path();
            if (!llvm::sys::fs::is_directory(path)) continue;
            std::string dirName = llvm::sys::path::filename(path).str();
            if (dirName.find("block") != 0) continue;

            std::string bf3File = path + "/bf2drmt.mlir";
            if (!llvm::sys::fs::exists(bf3File)) continue;

            auto mod = parseSourceFile<ModuleOp>(bf3File, context);
            if (!mod) {
                llvm::errs() << "  ✗ Cannot parse " << bf3File << "\n";
                return failure();
            }

            for (auto pipeOp : mod->getOps<bf2drmt::PipeOp>())
                blockInfos.push_back(eggifyPipeForMapper(pipeOp));
        }

        if (blockInfos.empty()) {
            llvm::errs() << "  ✗ No bf2drmt.mlir files found\n";
            return failure();
        }

        // Sort by block ID for deterministic output.
        llvm::sort(blockInfos, [](const MapperBlockInfo &a, const MapperBlockInfo &b) {
            return a.blockId < b.blockId;
        });

        // ── Build predecessor map from successor lists ─────────────────────
        std::map<int, std::vector<int>> predecessors;
        for (auto &info : blockInfos)
            for (int succ : info.successors)
                if (succ >= 0)
                    predecessors[succ].push_back(info.blockId);

        // ── Write mapper.egg ───────────────────────────────────────────────
        std::string mapperFile = inputDir + "/mapper.egg";
        std::error_code EC;
        llvm::raw_fd_ostream out(mapperFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot open " << mapperFile
                         << ": " << EC.message() << "\n";
            return failure();
        }

        // Header: hardware topology (BF3 NIC ASIC).
        out << ";; ============================================================\n"
            << ";; Mapper input generated by nutcracker VDRMTToBF2DRMTPass\n"
            << ";; ============================================================\n\n"
            << ";; Hardware configuration (BlueField-3)\n"
            << "(hardware DRMT :cores 1 :sigma 0.0)\n"
            << "(hardware DPA  :cores 190 :sigma 0.5)\n"
            << "(hardware ARM  :cores 16  :sigma 1.0)\n\n"
            << ";; Communication links\n"
            << "(link DRMT DPA  1.0 1.0)\n"
            << "(link DRMT ARM  1.0 1.0)\n"
            << "(link DPA  ARM  0.5 0.5)\n\n"
            << ";; bf2drmt op cost definitions\n"
            << "(function bf2drmt_struct_extract_ref (Type) Type :cost 1 :type stateless)\n"
            << "(function bf2drmt_read               (Type) Type :cost 1 :type stateless)\n"
            << "(function bf2drmt_constant           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf2drmt_binop              (Type) Type :cost 2 :type stateless)\n"
            << "(function bf2drmt_assign             (Type) Type :cost 1 :type stateful :sigma 0.1)\n"
            << "(function bf2drmt_match_key          (Type) Type :cost 2 :type stateless)\n"
            << "(function bf2drmt_cast               (Type) Type :cost 1 :type stateless)\n"
            << "(function bf2drmt_shl                (Type) Type :cost 2 :type stateless)\n"
            << "(function bf2drmt_shr                (Type) Type :cost 2 :type stateless)\n"
            << "(function bf2drmt_slice              (Type) Type :cost 1 :type stateless)\n"
            << "(function bf2drmt_concat             (Type) Type :cost 2 :type stateless)\n\n";

        // Per-block sections.
        for (auto &info : blockInfos) {
            std::string blkId   = "blk" + std::to_string(info.blockId);
            std::string implName = blkId + "_drmt";

            out << ";; ── Block " << info.blockId << " ──────────────────────────────────\n";
            for (auto &[name, expr] : info.ops)
                out << "(let " << name << "\n    " << expr << ")\n";

            out << "(let " << implName << "_ops (vec-of";
            for (auto &[name, _] : info.ops)
                out << " " << name;
            out << "))\n";

            // Predecessor list: "(blkX blkY ...)" or "()"
            auto &preds = predecessors[info.blockId];
            out << "(implement " << implName << " DRMT (";
            for (size_t i = 0; i < preds.size(); ++i) {
                if (i > 0) out << " ";
                out << "blk" << preds[i];
            }
            out << ") " << implName << "_ops)\n\n";
        }

        return success();
    }

    // ── DOCA Flow codegen (delegated to BF2DRMTToDocaFlow) ──────────────────
    // NOTE: called here for the vdsa_output/ preview copy only.
    // EmitHandlerCodePass re-runs this with the full hardware mapping and ARM
    // queue assignments to produce the authoritative deploy/doca_flow_pipeline.c.
    LogicalResult generateDocaFlowCodeBF2() {
        return mlir::generateDocaFlowCodeBF2(context, inputDir, inputDir, {}, {});
    }


    LogicalResult generateBlockBF2DRMT(StringRef vdrmtFile,
                                       StringRef bf2drmtFile) {
        // ── Phase 0: parse the vdrmt.mlir ──────────────────────────────────
        auto vdrmtMod = parseSourceFile<ModuleOp>(vdrmtFile, context);
        if (!vdrmtMod) {
            llvm::errs() << "  ✗ Failed to parse " << vdrmtFile << "\n";
            return failure();
        }

        // ── Phase 0b: run VFFSCoarseGrainedPass to lift counter decls ──────
        {
            PassManager pm(vdrmtMod->getContext());
            pm.addPass(mlir::createVFFSCoarseGrainedPass());
            // Non-fatal: ignore errors (no counter ops means nothing to do).
            (void)pm.run(*vdrmtMod);
        }

        // ── Phase 1: DialEgg op-level conversion ───────────────────────────
        std::string eggFilePath = "egglog_rules/vdrmt_to_bf2drmt.egg";
        if (!llvm::sys::fs::exists(eggFilePath)) {
            llvm::errs() << "  ✗ Egg template not found: " << eggFilePath << "\n";
            return failure();
        }

        auto supportedOps = loadSupportedOps(eggFilePath);
        EgglogCustomDefs customDefs = buildVDRMTCustomDefs(context);

        llvm::SmallVector<func::FuncOp> funcs(
            vdrmtMod->getOps<func::FuncOp>());
        for (auto funcOp : funcs) {
            if (failed(runDialEggOnFunction(funcOp, eggFilePath,
                                            vdrmtFile.str(),
                                            supportedOps, customDefs))) {
                llvm::errs() << "  ✗ DialEgg conversion failed for "
                             << vdrmtFile << "\n";
                return failure();
            }
        }

        // ── Phase 2: structural conversion (func.func → bf2drmt.pipe) ──────
        if (failed(restructureToBF2DRMTPipes(*vdrmtMod))) {
            llvm::errs() << "  ✗ Structural conversion failed for "
                         << vdrmtFile << "\n";
            return failure();
        }

        // ── Phase 2b: lower vFFS counter ops → bf2drmt counter ops ─────────
        {
            OpBuilder b(vdrmtMod->getContext());
            lowerVFFSCounters(*vdrmtMod, b);
        }

        // ── Phase 2c: remove residual vFFS hash5tuple ops ───────────────
        lowerVFFSHash5Tuple(*vdrmtMod);

        // ── Phase 2d: lower vDRMT meter ops → bf2drmt meter ops ─────────
        {
            OpBuilder b(vdrmtMod->getContext());
            lowerVDRMTMeters(*vdrmtMod, b);
        }

        // ── Phase 2e: reject vdrmt.register.* (BF2 unsupported until Stage 2/3) ─
        {
            OpBuilder b(vdrmtMod->getContext());
            if (failed(lowerVDRMTRegisters(*vdrmtMod, b)))
                return failure();
        }

        // ── Write output ────────────────────────────────────────────────────
        std::error_code EC;
        llvm::raw_fd_ostream outFile(bf2drmtFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot open " << bf2drmtFile
                         << ": " << EC.message() << "\n";
            return failure();
        }
        vdrmtMod->print(outFile);
        return success();
    }
};

// ============================================================================
// VDRMTToBF2DRMTPass
// ============================================================================

struct VDRMTToBF2DRMTPass
    : public PassWrapper<VDRMTToBF2DRMTPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(VDRMTToBF2DRMTPass)

    VDRMTToBF2DRMTPass() = default;
    VDRMTToBF2DRMTPass(const VDRMTToBF2DRMTPass &other) : PassWrapper(other) {}

    StringRef getArgument()    const final { return "vdrmt-to-bf2drmt"; }
    StringRef getDescription() const final {
        return "Fine-grained lowering: vDRMT ops → bf2drmt ops "
               "(DialEgg-based, generates bf2drmt.mlir per block)";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<vdrmt::vDRMTDialect>();
        registry.insert<bf2drmt::BF2DRMTDialect>();
        registry.insert<func::FuncDialect>();
    }

    void runOnOperation() override {
        auto *context = &getContext();

        llvm::outs() << "\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "  vDRMT → bf2drmt Fine-Grained Lowering Pass (DialEgg)\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

        std::string inputDir = "vdsa_output";

        llvm::SmallString<256> cwd;
        llvm::sys::fs::current_path(cwd);
        llvm::outs() << "Working directory: " << cwd << "\n";
        llvm::outs() << "Input directory:   " << inputDir << "\n\n";

        if (!llvm::sys::fs::exists(inputDir)) {
            llvm::errs() << "❌ Input directory not found: " << inputDir << "\n";
            signalPassFailure();
            return;
        }

        BF2DRMTBlockGenerator generator(context, inputDir);
        if (failed(generator.generate())) {
            llvm::errs() << "❌ Failed to generate bf2drmt files\n";
            signalPassFailure();
            return;
        }

        llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "✅ bf2drmt lowering complete!\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    }
};

} // anonymous namespace

namespace mlir {

std::unique_ptr<Pass> createVDRMTToBF2DRMTPass() {
    return std::make_unique<VDRMTToBF2DRMTPass>();
}

void registerVDRMTToBF2DRMTPass() {
    PassRegistration<VDRMTToBF2DRMTPass>();
}

} // namespace mlir
