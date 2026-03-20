// ============================================================================
// File: lib/Pass/VDRMTToBF3DRMT/VDRMTToBF3DRMTPass.cpp
// vDRMT → bf3drmt Fine-Grained Lowering Pass (DialEgg-based)
//
// This pass implements the fine-grained, op-level lowering from the virtual
// dRMT (vDRMT) dialect to the BF3 NIC ASIC target dialect (bf3drmt) using
// the DialEgg equality saturation infrastructure.
//
// Workflow:
//   Phase 1 — op-level conversion via DialEgg:
//     1. Eggify every op in each vdrmt func.func using Egglog::eggifyOperation.
//     2. Inject the eggified ops into the vdrmt_to_bf3drmt.egg template at the
//        ";; OPS HERE ;;" marker.
//     3. Execute the egglog binary; equality saturation rewrites vdrmt ops to
//        bf3drmt ops using the rules in the template.
//     4. Read the extracted results from the ";; EXTRACTS HERE ;;" section and
//        call Egglog::parseOperation to materialise the bf3drmt MLIR ops,
//        replacing the original vdrmt ops in place.
//
//   Phase 2 — structural conversion (per-function):
//     Each func.func wrapper is replaced by a bf3drmt.pipe op.
//     For functions with a vdrmt.cmp → vdrmt.if pattern (table-based routing):
//       - The bf3drmt read chain that produces the compared value is placed in a
//         bf3drmt.table_key region, terminated by bf3drmt.match_key.
//       - The then-successor becomes a bf3drmt.pipe_action @_hit { next N }.
//       - The else-successor becomes a bf3drmt.pipe_action @_miss { next N }.
//     For action-only functions (no conditional routing):
//       - All ops are placed in a bf3drmt.pipe_action @_default body.
//       - The vdrmt.next successor becomes a bf3drmt.next terminator.
// ============================================================================

#include <fstream>
#include <map>
#include <string>

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/AsmParser/AsmParser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "Pass/VDRMTToBF3DRMTPass.h"
#include "Pass/BF3DRMTToDocaFlowPass.h"
#include "Pass/Egglog.h"
#include "Pass/Utils.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTTypes.h"

using namespace mlir;
using namespace mlir::vdrmt;

namespace {

// ============================================================================
// Custom EgglogCustomDefs for vDRMT / bf3drmt types
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

/// Recursively convert vdrmt types to their bf3drmt equivalents.
///   vdrmt::ReferenceType → bf3drmt::ReferenceType
///   vdrmt::StructType    → bf3drmt::StructType
///   vdrmt::HeaderType    → bf3drmt::HeaderType
///   everything else      → unchanged
static mlir::Type convertToBF3DRMTType(mlir::Type type) {
    if (auto refTy = mlir::dyn_cast<vdrmt::ReferenceType>(type))
        return bf3drmt::ReferenceType::get(
            refTy.getContext(), convertToBF3DRMTType(refTy.getObjectType()));
    if (auto sTy = mlir::dyn_cast<vdrmt::StructType>(type)) {
        llvm::SmallVector<vdrmt::FieldInfo> fields;
        for (const auto &f : sTy.getElements())
            fields.emplace_back(f.name, convertToBF3DRMTType(f.type), f.annotations);
        return bf3drmt::StructType::get(sTy.getContext(), sTy.getName(), fields,
                                        sTy.getAnnotations());
    }
    if (auto hTy = mlir::dyn_cast<vdrmt::HeaderType>(type)) {
        llvm::SmallVector<vdrmt::FieldInfo> fields;
        for (const auto &f : hTy.getElements())
            fields.emplace_back(f.name, convertToBF3DRMTType(f.type), f.annotations);
        return bf3drmt::HeaderType::get(hTy.getContext(), hTy.getName(), fields,
                                        hTy.getAnnotations());
    }
    return type;
}

/// Build the EgglogCustomDefs that handle vdrmt.ref / vdrmt.struct /
/// vdrmt.header (and their bf3drmt counterparts).
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
            // The stored string was produced from a vdrmt type — convert to bf3drmt.
            return convertToBF3DRMTType(parsed);
        };

    defs.typeParsers["vdrmt_struct"]   = parseTypeFromEscapedStr;
    defs.typeParsers["vdrmt_header"]   = parseTypeFromEscapedStr;
    defs.typeParsers["bf3drmt_struct"] = parseTypeFromEscapedStr;
    defs.typeParsers["bf3drmt_header"] = parseTypeFromEscapedStr;

    // vdrmt_ref  →  vdrmt::ReferenceType
    auto parseVdrmtRef =
        [ctx](const std::vector<std::string> &split, Egglog &egglog) -> mlir::Type {
            mlir::Type inner = egglog.parseType(split[1]);
            return vdrmt::ReferenceType::get(ctx, inner);
        };

    // bf3drmt_ref  →  bf3drmt::ReferenceType
    auto parseRef =
        [ctx](const std::vector<std::string> &split, Egglog &egglog) -> mlir::Type {
            mlir::Type inner = egglog.parseType(split[1]);
            return bf3drmt::ReferenceType::get(ctx, inner);
        };

    defs.typeParsers["vdrmt_ref"]   = parseVdrmtRef;
    defs.typeParsers["bf3drmt_ref"] = parseRef;

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
            // fullName is e.g. "bf3drmt_struct_extract_ref"
            // parseOperation does:
            //   (1) find(opName)   — key uses underscores: "bf3drmt_struct_extract_ref"
            //   (2) replace _ → .  — key becomes:          "bf3drmt.struct.extract.ref"
            //   (3) at(opName)     — must also exist
            // So register under both forms.
            std::string underscoreKey = def.fullName;  // e.g. "bf3drmt_struct_extract_ref"
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

    return success();
}

// ============================================================================
// Phase 2: structural conversion helpers
// ============================================================================

/// Collect, in execution order, all bf3drmt.struct_extract_ref and
/// bf3drmt.read ops that transitively define 'val'. Stops at block arguments.
static void collectKeyChain(Value val,
                             llvm::SmallVectorImpl<Operation *> &result) {
    llvm::SmallPtrSet<Operation *, 8> seen;
    llvm::SmallVector<Value> worklist = {val};

    while (!worklist.empty()) {
        Value v = worklist.pop_back_val();
        Operation *defOp = v.getDefiningOp();
        if (!defOp) continue;
        if (!seen.insert(defOp).second) continue;
        if (isa<bf3drmt::StructExtractRefOp>(defOp) ||
            isa<bf3drmt::ReadOp>(defOp)) {
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

/// Convert a single (DialEgg-lowered) func.func into a bf3drmt.pipe.
///
/// After DialEgg, the function body may still contain vdrmt.cmp / vdrmt.if
/// (not lowered by DialEgg — they are structural) plus bf3drmt ops created
/// by DialEgg. Everything else should already be bf3drmt after DialEgg.
static LogicalResult convertFuncToPipe(func::FuncOp funcOp) {
    MLIRContext *ctx = funcOp.getContext();
    Block &funcEntry = funcOp.getFunctionBody().front();

    // Locate structural vdrmt ops (at function body top level only).
    vdrmt::CmpOp  cmpOp      = nullptr;
    vdrmt::IfOp   ifOp       = nullptr;
    vdrmt::NextOp topNextOp  = nullptr;

    for (auto &op : funcEntry) {
        if (!cmpOp)     cmpOp     = dyn_cast<vdrmt::CmpOp>(&op);
        if (!ifOp)      ifOp      = dyn_cast<vdrmt::IfOp>(&op);
        if (!topNextOp) topNextOp = dyn_cast<vdrmt::NextOp>(&op);
    }

    // Create the bf3drmt.pipe op immediately before the func.
    OpBuilder builder(funcOp);
    auto pipeOp = builder.create<bf3drmt::PipeOp>(
        funcOp.getLoc(),
        funcOp.getName(),
        bf3drmt::BF3DRMTPipeType::Basic,
        /*match_mask=*/nullptr,
        /*nr_entries=*/nullptr,
        /*annotations=*/nullptr);

    // The generated builder does NOT create blocks; add the entry block.
    Block &pipeEntry = pipeOp.getBody().emplaceBlock();

    // Mirror the function's block arguments onto the pipe body.
    IRMapping mapping;
    for (BlockArgument arg : funcEntry.getArguments()) {
        BlockArgument pipeArg =
            pipeEntry.addArgument(convertToBF3DRMTType(arg.getType()), arg.getLoc());
        mapping.map(arg, pipeArg);
    }

    builder.setInsertionPointToStart(&pipeEntry);

    if (cmpOp && ifOp) {
        // ── Table-based routing: cmp → if ────────────────────────────────
        // Collect the bf3drmt read chain that produces the compared value
        // and place it in a table_key region.

        Value keyVal = cmpOp.getLhs();

        llvm::SmallVector<Operation *> keyOps;
        collectKeyChain(keyVal, keyOps);

        // Build table_key and populate its body.
        auto tableKeyOp = builder.create<bf3drmt::TableKeyOp>(funcOp.getLoc());
        Block &tkBlock  = tableKeyOp.getBody().emplaceBlock();
        OpBuilder tkBuilder(ctx);
        tkBuilder.setInsertionPointToStart(&tkBlock);

        IRMapping tkMapping(mapping);
        for (Operation *op : keyOps)
            tkBuilder.clone(*op, tkMapping);

        Value clonedKey = tkMapping.lookupOrDefault(keyVal);
        tkBuilder.create<bf3drmt::MatchKeyOp>(cmpOp.getLoc(), clonedKey);

        // Build pipe_action @_hit { bf3drmt.next N_then }.
        auto [thenSucc, elseSucc] = getIfSuccessors(ifOp);

        auto hitSym    = mlir::SymbolRefAttr::get(ctx, "_hit");
        auto hitAction = builder.create<bf3drmt::PipeActionOp>(
            funcOp.getLoc(), hitSym, /*annotations=*/nullptr);
        Block &hitBlock = hitAction.getBody().emplaceBlock();
        OpBuilder hitBuilder(ctx);
        hitBuilder.setInsertionPointToStart(&hitBlock);
        hitBuilder.create<bf3drmt::NextOp>(funcOp.getLoc(), thenSucc);

        // Build pipe_action @_miss { bf3drmt.next N_else }.
        auto missSym    = mlir::SymbolRefAttr::get(ctx, "_miss");
        auto missAction = builder.create<bf3drmt::PipeActionOp>(
            funcOp.getLoc(), missSym, /*annotations=*/nullptr);
        Block &missBlock = missAction.getBody().emplaceBlock();
        OpBuilder missBuilder(ctx);
        missBuilder.setInsertionPointToStart(&missBlock);
        missBuilder.create<bf3drmt::NextOp>(funcOp.getLoc(), elseSucc);

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
        auto actionOp = builder.create<bf3drmt::PipeActionOp>(
            funcOp.getLoc(), defSym, /*annotations=*/nullptr);
        Block &actionBlock = actionOp.getBody().emplaceBlock();
        OpBuilder actionBuilder(ctx);
        actionBuilder.setInsertionPointToStart(&actionBlock);

        for (Operation *op : opsToClone)
            actionBuilder.clone(*op, mapping);

        actionBuilder.create<bf3drmt::NextOp>(funcOp.getLoc(), nextSucc);
    }

    funcOp.erase();
    return success();
}

/// Replace every func.func in 'mod' with a bf3drmt.pipe.
static LogicalResult restructureToBF3DRMTPipes(ModuleOp mod) {
    llvm::SmallVector<func::FuncOp> funcs(mod.getOps<func::FuncOp>());
    for (auto funcOp : funcs)
        if (failed(convertFuncToPipe(funcOp)))
            return failure();
    return success();
}

// ============================================================================
// Phase 3: Mapper egglog emitter
// ============================================================================

/// Serialize a bf3drmt (or vdrmt / builtin integer) type to its mapper egglog form.
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
    if (auto refTy = mlir::dyn_cast<bf3drmt::ReferenceType>(type))
        return "(bf3drmt_ref " + typeToMapperEgglog(refTy.getObjectType()) + ")";
    if (mlir::isa<bf3drmt::StructType>(type)) {
        std::string s; llvm::raw_string_ostream os(s); type.print(os);
        return "(bf3drmt_struct \"" + escapeEgglogString(s) + "\")";
    }
    if (mlir::isa<bf3drmt::HeaderType>(type)) {
        std::string s; llvm::raw_string_ostream os(s); type.print(os);
        return "(bf3drmt_header \"" + escapeEgglogString(s) + "\")";
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

/// Convert a single bf3drmt op to its mapper egglog expression (no `let` prefix).
/// Returns "" for structural ops (pipe, pipe_action, table_key, next) that are
/// not included in the op cost model.
static std::string bf3drmtOpToMapperEgglog(
    mlir::Operation &op,
    const llvm::DenseMap<mlir::Value, std::string> &names)
{
    // Look up SSA value — block args are stored inline as "(Value N <type>)".
    auto val = [&](mlir::Value v) -> std::string {
        auto it = names.find(v);
        return (it != names.end()) ? it->second : "(OpaqueValue)";
    };

    if (auto serOp = mlir::dyn_cast<bf3drmt::StructExtractRefOp>(&op)) {
        return "(bf3drmt_struct_extract_ref "
               + val(serOp.getInput())
               + " (NamedAttr \"fieldIndex\" (IntegerAttr "
               + std::to_string(serOp.getFieldIndex()) + " (I32))) "
               + typeToMapperEgglog(serOp.getResult().getType()) + ")";
    }
    if (auto readOp = mlir::dyn_cast<bf3drmt::ReadOp>(&op)) {
        return "(bf3drmt_read "
               + val(readOp.getRef())
               + " " + typeToMapperEgglog(readOp.getResult().getType()) + ")";
    }
    if (auto constOp = mlir::dyn_cast<bf3drmt::ConstantOp>(&op)) {
        return "(bf3drmt_constant (NamedAttr \"value\" "
               + attrToMapperEgglog(constOp.getValue()) + ") "
               + typeToMapperEgglog(constOp.getResult().getType()) + ")";
    }
    if (auto binOp = mlir::dyn_cast<bf3drmt::BinOp>(&op)) {
        return "(bf3drmt_binop "
               + val(binOp.getLhs()) + " " + val(binOp.getRhs())
               + " (NamedAttr \"kind\" (IntegerAttr "
               + std::to_string(static_cast<int>(binOp.getKind())) + " (I32))) "
               + typeToMapperEgglog(binOp.getResult().getType()) + ")";
    }
    if (auto assignOp = mlir::dyn_cast<bf3drmt::AssignOp>(&op)) {
        return "(bf3drmt_assign "
               + val(assignOp.getValue()) + " "
               + val(assignOp.getRef()) + ")";
    }
    if (auto matchOp = mlir::dyn_cast<bf3drmt::MatchKeyOp>(&op)) {
        return "(bf3drmt_match_key "
               + val(matchOp.getKey()) + ")";
    }
    if (auto castOp = mlir::dyn_cast<bf3drmt::CastOp>(&op)) {
        return "(bf3drmt_cast "
               + val(castOp.getValue())
               + " " + typeToMapperEgglog(castOp.getResult().getType()) + ")";
    }
    if (auto shlOp = mlir::dyn_cast<bf3drmt::ShlOp>(&op)) {
        return "(bf3drmt_shl "
               + val(shlOp.getLhs()) + " " + val(shlOp.getRhs())
               + " " + typeToMapperEgglog(shlOp.getResult().getType()) + ")";
    }
    if (auto shrOp = mlir::dyn_cast<bf3drmt::ShrOp>(&op)) {
        return "(bf3drmt_shr "
               + val(shrOp.getLhs()) + " " + val(shrOp.getRhs())
               + " " + typeToMapperEgglog(shrOp.getResult().getType()) + ")";
    }
    if (auto sliceOp = mlir::dyn_cast<bf3drmt::SliceOp>(&op)) {
        return "(bf3drmt_slice "
               + val(sliceOp.getInput())
               + " (NamedAttr \"hi\" (IntegerAttr "
               + std::to_string(sliceOp.getHighBit()) + " (I32)))"
               + " (NamedAttr \"lo\" (IntegerAttr "
               + std::to_string(sliceOp.getLowBit()) + " (I32)))"
               + " " + typeToMapperEgglog(sliceOp.getResult().getType()) + ")";
    }
    if (auto concatOp = mlir::dyn_cast<bf3drmt::ConcatOp>(&op)) {
        return "(bf3drmt_concat "
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

/// Walk all regions of a bf3drmt.pipe and collect ops for the mapper.
static MapperBlockInfo eggifyPipeForMapper(bf3drmt::PipeOp pipeOp) {
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
        if (mlir::isa<bf3drmt::PipeOp, bf3drmt::PipeActionOp,
                      bf3drmt::TableKeyOp>(op))
            return mlir::WalkResult::advance();

        // Collect successors from bf3drmt.next.
        if (auto nextOp = mlir::dyn_cast<bf3drmt::NextOp>(op)) {
            info.successors.push_back(nextOp.getSuccessor());
            return mlir::WalkResult::advance();
        }

        std::string expr = bf3drmtOpToMapperEgglog(*op, names);
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
// BF3DRMT Block Generator
// ============================================================================

class BF3DRMTBlockGenerator {
public:
    BF3DRMTBlockGenerator(MLIRContext *context, StringRef inputDir)
        : context(context), inputDir(inputDir.str()) {}

    LogicalResult generate() {
        int successCount = 0, skipCount = 0;

        std::error_code ec;
        for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
             dir != end && !ec; dir.increment(ec)) {

            auto path = dir->path();
            if (!llvm::sys::fs::is_directory(path)) continue;

            std::string dirName = llvm::sys::path::filename(path).str();
            if (dirName.find("block") != 0) continue;

            std::string vdrmtFile   = path + "/vdrmt.mlir";
            std::string bf3drmtFile = path + "/bf3drmt.mlir";

            if (!llvm::sys::fs::exists(vdrmtFile)) {
                llvm::outs() << "  ⊘ Skipping " << dirName << " (no vdrmt.mlir)\n";
                skipCount++;
                continue;
            }

            if (failed(generateBlockBF3DRMT(vdrmtFile, bf3drmtFile))) {
                llvm::errs() << "  ✗ Failed to generate "
                             << dirName << "/bf3drmt.mlir\n";
                return failure();
            }
            successCount++;
            llvm::outs() << "  ✓ Generated " << dirName << "/bf3drmt.mlir\n";
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
        if (failed(generateDocaFlowCode()))
            return failure();
        llvm::outs() << "  ✓ Generated doca_flow_pipeline.c\n";

        return success();
    }

private:
    MLIRContext *context;
    std::string inputDir;

    LogicalResult generateMapperEgglog() {
        // ── Collect per-block info from all generated bf3drmt.mlir files ──
        std::vector<MapperBlockInfo> blockInfos;

        std::error_code ec;
        for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
             dir != end && !ec; dir.increment(ec)) {

            auto path = dir->path();
            if (!llvm::sys::fs::is_directory(path)) continue;
            std::string dirName = llvm::sys::path::filename(path).str();
            if (dirName.find("block") != 0) continue;

            std::string bf3File = path + "/bf3drmt.mlir";
            if (!llvm::sys::fs::exists(bf3File)) continue;

            auto mod = parseSourceFile<ModuleOp>(bf3File, context);
            if (!mod) {
                llvm::errs() << "  ✗ Cannot parse " << bf3File << "\n";
                return failure();
            }

            for (auto pipeOp : mod->getOps<bf3drmt::PipeOp>())
                blockInfos.push_back(eggifyPipeForMapper(pipeOp));
        }

        if (blockInfos.empty()) {
            llvm::errs() << "  ✗ No bf3drmt.mlir files found\n";
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
            << ";; Mapper input generated by nutcracker VDRMTToBF3DRMTPass\n"
            << ";; ============================================================\n\n"
            << ";; Hardware configuration (BlueField-3)\n"
            << "(hardware DRMT :cores 1 :sigma 0.0)\n"
            << "(hardware DPA  :cores 190 :sigma 0.5)\n"
            << "(hardware ARM  :cores 16  :sigma 1.0)\n\n"
            << ";; Communication links\n"
            << "(link DRMT DPA  1.0 1.0)\n"
            << "(link DRMT ARM  1.0 1.0)\n"
            << "(link DPA  ARM  0.5 0.5)\n\n"
            << ";; bf3drmt op cost definitions\n"
            << "(function bf3drmt_struct_extract_ref (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3drmt_read               (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3drmt_constant           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3drmt_binop              (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3drmt_assign             (Type) Type :cost 1 :type stateful :sigma 0.1)\n"
            << "(function bf3drmt_match_key          (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3drmt_cast               (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3drmt_shl                (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3drmt_shr                (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3drmt_slice              (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3drmt_concat             (Type) Type :cost 2 :type stateless)\n\n";

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

    // ── DOCA Flow codegen (delegated to BF3DRMTToDocaFlow) ──────────────────
    // NOTE: called here for the vdsa_output/ preview copy only.
    // EmitHandlerCodePass re-runs this with the full hardware mapping and ARM
    // queue assignments to produce the authoritative deploy/doca_flow_pipeline.c.
    LogicalResult generateDocaFlowCode() {
        return mlir::generateDocaFlowCode(context, inputDir, inputDir, {}, {});
    }


    LogicalResult generateBlockBF3DRMT(StringRef vdrmtFile,
                                       StringRef bf3drmtFile) {
        // ── Phase 0: parse the vdrmt.mlir ──────────────────────────────────
        auto vdrmtMod = parseSourceFile<ModuleOp>(vdrmtFile, context);
        if (!vdrmtMod) {
            llvm::errs() << "  ✗ Failed to parse " << vdrmtFile << "\n";
            return failure();
        }

        // ── Phase 1: DialEgg op-level conversion ───────────────────────────
        std::string eggFilePath = "egglog_rules/vdrmt_to_bf3drmt.egg";
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

        // ── Phase 2: structural conversion (func.func → bf3drmt.pipe) ──────
        if (failed(restructureToBF3DRMTPipes(*vdrmtMod))) {
            llvm::errs() << "  ✗ Structural conversion failed for "
                         << vdrmtFile << "\n";
            return failure();
        }

        // ── Write output ────────────────────────────────────────────────────
        std::error_code EC;
        llvm::raw_fd_ostream outFile(bf3drmtFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot open " << bf3drmtFile
                         << ": " << EC.message() << "\n";
            return failure();
        }
        vdrmtMod->print(outFile);
        return success();
    }
};

// ============================================================================
// VDRMTToBF3DRMTPass
// ============================================================================

struct VDRMTToBF3DRMTPass
    : public PassWrapper<VDRMTToBF3DRMTPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(VDRMTToBF3DRMTPass)

    VDRMTToBF3DRMTPass() = default;
    VDRMTToBF3DRMTPass(const VDRMTToBF3DRMTPass &other) : PassWrapper(other) {}

    StringRef getArgument()    const final { return "vdrmt-to-bf3drmt"; }
    StringRef getDescription() const final {
        return "Fine-grained lowering: vDRMT ops → bf3drmt ops "
               "(DialEgg-based, generates bf3drmt.mlir per block)";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<vdrmt::vDRMTDialect>();
        registry.insert<bf3drmt::BF3DRMTDialect>();
        registry.insert<func::FuncDialect>();
    }

    void runOnOperation() override {
        auto *context = &getContext();

        llvm::outs() << "\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "  vDRMT → bf3drmt Fine-Grained Lowering Pass (DialEgg)\n";
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

        BF3DRMTBlockGenerator generator(context, inputDir);
        if (failed(generator.generate())) {
            llvm::errs() << "❌ Failed to generate bf3drmt files\n";
            signalPassFailure();
            return;
        }

        llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "✅ bf3drmt lowering complete!\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    }
};

} // anonymous namespace

namespace mlir {

std::unique_ptr<Pass> createVDRMTToBF3DRMTPass() {
    return std::make_unique<VDRMTToBF3DRMTPass>();
}

void registerVDRMTToBF3DRMTPass() {
    PassRegistration<VDRMTToBF3DRMTPass>();
}

} // namespace mlir
