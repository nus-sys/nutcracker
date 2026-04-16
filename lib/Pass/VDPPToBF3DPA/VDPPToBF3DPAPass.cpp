// ============================================================================
// File: lib/Pass/VDPPToBF3DPA/VDPPToBF3DPAPass.cpp
// vDPP → bf3dpa / arm.ll Fine-Grained Lowering Pass (DialEgg-based)
//
// Workflow:
//   Phase 1 — op-level conversion via DialEgg:
//     Eggify every data-flow op in the vdpp func.func, run the
//     vdpp_to_bf3dpa.egg template through egglog, parse back bf3dpa ops.
//     Control-flow ops (br, cond_br, return) are kept as opaque.
//
//   Phase 2 — write bf3dpa.mlir per block directory.
//             write arm.ll per block directory (vDPP → LLVM IR for ARM).
//
//   Phase 3 — append DPA block entries to mapper.egg.
// ============================================================================

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/AsmParser/AsmParser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "Pass/VDPPToBF3DPAPass.h"
#include "Pass/VDPPToLLVMPass.h"
#include "Pass/Egglog.h"
#include "Pass/Utils.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h"

using namespace mlir;

namespace {

// ============================================================================
// Egglog string helpers (same approach as VDRMTToBF3DRMTPass)
// ============================================================================

static std::string escapeEgglogString(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') r += "__Q__";
        else          r += c;
    }
    return r;
}

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

// ============================================================================
// Custom EgglogCustomDefs for vDPP / bf3dpa types
// ============================================================================

static EgglogCustomDefs buildVDPPCustomDefs(MLIRContext *ctx) {
    EgglogCustomDefs defs;

    // ── Type stringifiers ──────────────────────────────────────────────────

    // vdpp.ptr  →  (vdpp_ptr <inner_type>)
    defs.typeStringifiers["vdpp.ptr"] =
        [](mlir::Type type, Egglog &egglog) -> std::vector<std::string> {
            auto ptrType = mlir::cast<vdpp::PointerType>(type);
            return {"vdpp_ptr", egglog.eggifyType(ptrType.getElementType())};
        };

    // vdpp.struct  →  (vdpp_struct "escaped_mlir_type_string")
    defs.typeStringifiers["vdpp.struct"] =
        [](mlir::Type type, Egglog &) -> std::vector<std::string> {
            std::string typeStr;
            llvm::raw_string_ostream os(typeStr);
            type.print(os);
            os.flush();
            return {"vdpp_struct", "\"" + escapeEgglogString(typeStr) + "\""};
        };

    // ── Type parsers ───────────────────────────────────────────────────────

    // Parse a vdpp.ptr from (vdpp_ptr <inner>) or (bf3dpa_ptr <inner>)
    auto parsePtrType =
        [ctx](const std::vector<std::string> &split, Egglog &egglog) -> mlir::Type {
            mlir::Type inner = egglog.parseType(split[1]);
            return vdpp::PointerType::get(ctx, inner);
        };

    // Parse a struct type from the escaped string
    auto parseStructType =
        [ctx](const std::vector<std::string> &split, Egglog &) -> mlir::Type {
            const std::string &raw = split[1];
            std::string inner = (raw.size() >= 2 &&
                                 raw.front() == '"' && raw.back() == '"')
                                    ? raw.substr(1, raw.size() - 2) : raw;
            std::string typeStr = unescapeEgglogString(inner);
            return mlir::parseType(typeStr, ctx);
        };

    defs.typeParsers["vdpp_ptr"]    = parsePtrType;
    defs.typeParsers["bf3dpa_ptr"]  = parsePtrType;
    defs.typeParsers["vdpp_struct"] = parseStructType;
    defs.typeParsers["bf3dpa_struct"] = parseStructType;

    return defs;
}

// ============================================================================
// DialEgg helpers (mirrored from VDRMTToBF3DRMTPass)
// ============================================================================

static std::map<std::string, EgglogOpDef>
loadSupportedOps(const std::string &eggFilePath) {
    std::map<std::string, EgglogOpDef> ops;
    std::ifstream f(eggFilePath);
    std::string line;
    while (std::getline(f, line)) {
        if (EgglogOpDef::isOpFunction(line)) {
            EgglogOpDef def = EgglogOpDef::parseOpFunction(line);
            std::string underscoreKey = def.fullName;
            std::string dotKey = underscoreKey;
            std::replace(dotKey.begin(), dotKey.end(), '_', '.');
            ops.emplace(underscoreKey, def);
            ops.emplace(dotKey, def);
        }
    }
    return ops;
}

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

    const char *egglogEnv = std::getenv("EGGLOG");
    std::string egglogBin;
    if (egglogEnv) {
        egglogBin = std::string(egglogEnv) + "/egglog";
    } else if (llvm::sys::fs::exists("third_party/egglog/target/release/egglog")) {
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
        llvm::errs() << "  ✗ egglog exited with code " << ret << "\n";
        return failure();
    }
    return success();
}

// ============================================================================
// DialEgg: eggify and replace one func.func
// ============================================================================

/// Determine if an op should be eggified (data-flow) or kept opaque
/// (control-flow terminators that egglog cannot reason about).
static bool isControlFlowOp(mlir::Operation *op) {
    // Hash5TupleApplyOp carries a symbol-ref attribute that egglog cannot
    // represent; skip it here and lower it directly in the post-processing step.
    return mlir::isa<vdpp::BranchOp, vdpp::CondBranchOp,
                     vdpp::ReturnOp, func::ReturnOp,
                     vdpp::Hash5TupleApplyOp>(op);
}

static LogicalResult runDialEggOnFunction(
    func::FuncOp funcOp,
    const std::string &eggFilePath,
    const std::string &vdppFilePath,
    const std::map<std::string, EgglogOpDef> &supportedOps,
    const EgglogCustomDefs &customDefs) {

    MLIRContext *ctx = funcOp.getContext();
    Egglog egglog(*ctx, customDefs, supportedOps);

    // Register function block arguments as opaque values.
    for (mlir::Value arg : funcOp.getArguments())
        egglog.eggifyValue(arg);

    // Eggify data-flow ops; treat control-flow ops as opaque.
    for (mlir::Block &block : funcOp.getFunctionBody().getBlocks())
        for (mlir::Operation &op : block.getOperations())
            if (!isControlFlowOp(&op))
                egglog.eggifyOperation(&op);

    std::string base         = vdppFilePath.substr(0, vdppFilePath.find(".mlir"));
    std::string extractFile  = base + "-egglog-extract.log";
    std::string logFile      = base + "-egglog.log";
    std::string opsEggFile   = base + ".ops.egg";

    if (failed(runEgglogOnFile(eggFilePath, egglog.eggifiedBlock,
                               funcOp.getName().str(),
                               opsEggFile, extractFile, logFile)))
        return failure();

    std::ifstream file(extractFile);
    if (!file.is_open()) {
        llvm::errs() << "  ✗ Cannot open extract file: " << extractFile << "\n";
        return failure();
    }

    for (const EggifiedOp *eggOp : egglog.eggifiedBlock) {
        if (!eggOp->shouldBeExtracted()) continue;

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

    // Dead-code elimination.
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

    // Convert remaining vdpp control-flow ops to bf3dpa equivalents.
    // These were skipped by egglog (can't reason about CF) but must be
    // lowered before bf3dpa.mlir is valid.
    mlir::OpBuilder builder(ctx);
    for (mlir::Block &block : funcOp.getFunctionBody().getBlocks()) {
        for (auto it = block.begin(); it != block.end(); ) {
            mlir::Operation &op = *it++;

            if (auto retOp = mlir::dyn_cast<vdpp::ReturnOp>(op)) {
                builder.setInsertionPoint(&op);
                if (auto succ = retOp.getSuccessor())
                    builder.create<bf3dpa::ReturnOp>(retOp.getLoc(),
                                                     (int32_t)*succ);
                else
                    builder.create<bf3dpa::ReturnOp>(retOp.getLoc());
                retOp.erase();

            } else if (auto brOp = mlir::dyn_cast<vdpp::BranchOp>(op)) {
                builder.setInsertionPoint(&op);
                builder.create<bf3dpa::BranchOp>(
                    brOp.getLoc(), brOp.getDest(), brOp.getDestOperands());
                brOp.erase();

            } else if (auto condBr = mlir::dyn_cast<vdpp::CondBranchOp>(op)) {
                builder.setInsertionPoint(&op);
                builder.create<bf3dpa::CondBranchOp>(
                    condBr.getLoc(), condBr.getCondition(),
                    condBr.getTrueDest(), condBr.getFalseDest(),
                    condBr.getTrueDestOperands(),
                    condBr.getFalseDestOperands());
                condBr.erase();

            } else if (auto hashOp = mlir::dyn_cast<vdpp::Hash5TupleApplyOp>(op)) {
                // Lower vdpp.hash5tuple.apply → bf3dpa.hash5tuple.apply directly.
                // Skipped by egglog because it carries a symbol-ref attribute.
                builder.setInsertionPoint(&op);
                auto newOp = builder.create<bf3dpa::Hash5TupleApplyOp>(
                    hashOp.getLoc(),
                    hashOp.getResult().getType(),
                    hashOp.getInstanceAttr(),
                    hashOp.getSrcAddr(),
                    hashOp.getDstAddr(),
                    hashOp.getSrcPort(),
                    hashOp.getDstPort(),
                    hashOp.getProto());
                hashOp.getResult().replaceAllUsesWith(newOp.getResult());
                hashOp.erase();
            }
        }
    }
    return success();
}

// ============================================================================
// Mapper egglog helpers for bf3dpa blocks
// ============================================================================

/// A bf3dpa op's representation for mapper.egg.
struct DPAMapperBlockInfo {
    int blockId = -1;
    std::vector<int> successors; // from vdpp.return {successor = N}
    std::vector<std::pair<std::string, std::string>> ops; // (name, expr)
};

/// Map an integer type to a simple mapper cost tag.
static std::string bf3dpaOpMapperExpr(mlir::Operation &op) {
    std::string dialectName = op.getDialect()
        ? op.getDialect()->getNamespace().str() : "";
    if (dialectName != "bf3dpa") return "";

    llvm::StringRef opName = op.getName().stripDialect();
    // Result type as a simple tag (use I32 as placeholder for non-integer).
    std::string typetag = "I32";
    if (!op.getResultTypes().empty()) {
        mlir::Type t = op.getResultTypes()[0];
        if (auto ity = mlir::dyn_cast<mlir::IntegerType>(t)) {
            typetag = "I" + std::to_string(ity.getWidth());
        }
    }
    // Convert op name to a valid egglog function name: replace '.' with '_'.
    std::string opNameStr = opName.str();
    std::replace(opNameStr.begin(), opNameStr.end(), '.', '_');
    std::string funcName = "bf3dpa_" + opNameStr;
    return "(" + funcName + " (" + typetag + "))";
}

static DPAMapperBlockInfo eggifyFuncForMapper(func::FuncOp funcOp, int blockId) {
    DPAMapperBlockInfo info;
    info.blockId = blockId;
    std::string prefix = "blk" + std::to_string(blockId) + "_dpa";
    int opIdx = 0;

    funcOp->walk([&](mlir::Operation *op) {
        if (mlir::isa<func::FuncOp>(op)) return mlir::WalkResult::advance();

        // Collect successors from vdpp.return {successor = N}.
        if (auto retOp = mlir::dyn_cast<vdpp::ReturnOp>(op)) {
            if (auto succ = retOp.getSuccessor())
                info.successors.push_back((int32_t)succ.value());
            return mlir::WalkResult::advance();
        }

        std::string expr = bf3dpaOpMapperExpr(*op);
        if (expr.empty()) return mlir::WalkResult::advance();

        std::string opName = prefix + "_op" + std::to_string(opIdx++);
        info.ops.emplace_back(opName, expr);
        return mlir::WalkResult::advance();
    });

    return info;
}

// ============================================================================
// BF3DPA Block Generator (main driver)
// ============================================================================

class BF3DPABlockGenerator {
public:
    BF3DPABlockGenerator(MLIRContext *ctx, StringRef inputDir)
        : context(ctx), inputDir(inputDir.str()) {}

    LogicalResult generate() {
        // Locate the egglog template.
        const char *rootEnv = std::getenv("NUTCRACKER_ROOT");
        std::string eggFilePath = rootEnv
            ? (std::string(rootEnv) + "/egglog_rules/vdpp_to_bf3dpa.egg")
            : "egglog_rules/vdpp_to_bf3dpa.egg";

        if (!llvm::sys::fs::exists(eggFilePath)) {
            llvm::errs() << "  ✗ vdpp_to_bf3dpa.egg not found: " << eggFilePath << "\n";
            return failure();
        }

        auto supportedOps = loadSupportedOps(eggFilePath);
        auto customDefs   = buildVDPPCustomDefs(context);

        int successCount = 0, skipCount = 0;
        std::vector<DPAMapperBlockInfo> mapperInfos;

        std::error_code ec;
        // Collect block dirs sorted by name for deterministic output.
        std::vector<std::string> blockDirs;
        for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
             dir != end && !ec; dir.increment(ec)) {
            auto path = dir->path();
            if (!llvm::sys::fs::is_directory(path)) continue;
            std::string dirName = llvm::sys::path::filename(path).str();
            if (dirName.find("block") != 0) continue;
            blockDirs.push_back(path);
        }
        llvm::sort(blockDirs);

        for (const std::string &path : blockDirs) {
            std::string dirName = llvm::sys::path::filename(path).str();
            std::string vdppFile   = path + "/vdpp.mlir";
            std::string bf3dpaFile = path + "/bf3dpa.mlir";

            if (!llvm::sys::fs::exists(vdppFile)) {
                llvm::outs() << "  ⊘ Skipping " << dirName << " (no vdpp.mlir)\n";
                skipCount++;
                continue;
            }

            // Extract block ID from directory name (e.g., "block2" → 2).
            int blockId = -1;
            llvm::StringRef nameRef(dirName);
            if (nameRef.starts_with("block"))
                (void)nameRef.drop_front(5).getAsInteger(10, blockId);

            if (failed(generateBlockBF3DPA(vdppFile, bf3dpaFile, eggFilePath,
                                           supportedOps, customDefs,
                                           blockId, mapperInfos))) {
                llvm::errs() << "  ✗ Failed: " << dirName << "/bf3dpa.mlir\n";
                return failure();
            }
            llvm::outs() << "  ✓ Generated " << dirName << "/bf3dpa.mlir\n";

            // Also lower the original vDPP block to LLVM IR for the ARM target.
            // Both bf3dpa.mlir and arm.ll represent the same block on different
            // hardware — the mapper uses both to find the optimal assignment.
            std::string armLLFile = path + "/arm.ll";
            {
                std::error_code EC;
                llvm::raw_fd_ostream armOut(armLLFile, EC);
                if (EC) {
                    llvm::errs() << "  ✗ Cannot open " << armLLFile << "\n";
                    return failure();
                }
                if (failed(emitVDPPAsLLVMIR(blockId, vdppFile, armOut))) {
                    llvm::errs() << "  ✗ Failed: " << dirName << "/arm.ll"
                                 << " (non-fatal, continuing)\n";
                    // arm.ll failure does not block DPA mapper entry generation.
                }
            }
            llvm::outs() << "  ✓ Generated " << dirName << "/arm.ll\n";

            successCount++;
        }

        llvm::outs() << "\n  Summary:\n"
                     << "    ✓ Generated: " << successCount << " blocks\n";
        if (skipCount > 0)
            llvm::outs() << "    ⊘ Skipped:   " << skipCount << " blocks\n";

        // Phase 3: append DPA mapper entries to mapper.egg.
        llvm::outs() << "\n  Appending DPA entries to mapper.egg ...\n";
        if (failed(appendMapperEgglog(mapperInfos)))
            return failure();
        llvm::outs() << "  ✓ mapper.egg updated\n";

        // Note: C code generation (nc_types.h, dpa_handler.c, arm_handler.c)
        // is deferred to the --emit-handler-code pass which runs after the
        // mapper step so only blocks assigned to DPA/ARM are emitted.

        return success();
    }

private:
    MLIRContext *context;
    std::string inputDir;

    LogicalResult generateBlockBF3DPA(
        const std::string &vdppFile,
        const std::string &bf3dpaFile,
        const std::string &eggFilePath,
        const std::map<std::string, EgglogOpDef> &supportedOps,
        const EgglogCustomDefs &customDefs,
        int blockId,
        std::vector<DPAMapperBlockInfo> &mapperInfos) {

        auto mod = parseSourceFile<ModuleOp>(vdppFile, context);
        if (!mod) {
            llvm::errs() << "  ✗ Cannot parse " << vdppFile << "\n";
            return failure();
        }

        // Reject ARM-only VFFAs (SHA/RegEx/Compress) — DPA can't reach them.
        {
            mlir::Operation *offender = nullptr;
            mlir::StringRef opName;
            mod->walk([&](mlir::Operation *op) {
                if (offender) return;
                if (mlir::isa<vdpp::ShaInstanceOp, vdpp::ShaComputeOp,
                              vdpp::RegexInstanceOp, vdpp::RegexMatchOp,
                              vdpp::CompressInstanceOp, vdpp::CompressOp,
                              vdpp::DecompressOp>(op)) {
                    offender = op;
                    opName = op->getName().getStringRef();
                }
            });
            if (offender) {
                offender->emitError(
                    "BF3DPA lowering: ") << opName << " is ARM-only "
                    "(DPA firmware has no SHA/RegEx/Compress primitive). "
                    "Partition should have routed this to ARM (vDPP→LLVM).";
                return failure();
            }
        }

        // Run DialEgg on each func.func in the module.
        for (auto funcOp : mod->getOps<func::FuncOp>()) {
            if (failed(runDialEggOnFunction(funcOp, eggFilePath, vdppFile,
                                           supportedOps, customDefs)))
                return failure();
        }

        // Lower module-level vdpp.hash5tuple_instance → bf3dpa.hash5tuple_instance.
        // These declarations live outside func.func so DialEgg never sees them.
        {
            mlir::OpBuilder modBuilder(mod->getContext());
            llvm::SmallVector<mlir::Operation *> toErase;
            for (mlir::Operation &op : *mod->getBody()) {
                if (auto instOp = mlir::dyn_cast<vdpp::Hash5TupleInstanceOp>(op)) {
                    modBuilder.setInsertionPoint(&op);
                    modBuilder.create<bf3dpa::Hash5TupleInstanceOp>(
                        instOp.getLoc(), instOp.getSymNameAttr());
                    toErase.push_back(&op);
                }
            }
            for (auto *op : toErase) op->erase();
        }

        // Collect mapper info from the (now bf3dpa) module.
        for (auto funcOp : mod->getOps<func::FuncOp>())
            mapperInfos.push_back(eggifyFuncForMapper(funcOp, blockId));

        // Write bf3dpa.mlir.
        std::error_code EC;
        llvm::raw_fd_ostream outFile(bf3dpaFile, EC);
        if (EC) {
            llvm::errs() << "  ✗ Cannot write " << bf3dpaFile << "\n";
            return failure();
        }
        mod->print(outFile);
        return success();
    }

    LogicalResult appendMapperEgglog(
        const std::vector<DPAMapperBlockInfo> &infos) {

        if (infos.empty()) return success();

        // Sort by block ID.
        std::vector<DPAMapperBlockInfo> sorted = infos;
        llvm::sort(sorted, [](const DPAMapperBlockInfo &a,
                               const DPAMapperBlockInfo &b) {
            return a.blockId < b.blockId;
        });

        // Build predecessor map.
        std::map<int, std::vector<int>> predecessors;
        for (auto &info : sorted)
            for (int succ : info.successors)
                if (succ >= 0)
                    predecessors[succ].push_back(info.blockId);

        std::string mapperFile = inputDir + "/mapper.egg";
        // Append to existing mapper.egg (DRMT blocks already written there).
        std::ofstream out(mapperFile, std::ios::app);
        if (!out.is_open()) {
            llvm::errs() << "  ✗ Cannot open " << mapperFile << " for append\n";
            return failure();
        }

        out << "\n;; bf3dpa op cost definitions\n"
            << "(function bf3dpa_constant      (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_add           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_sub           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_mul           (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3dpa_div           (Type) Type :cost 3 :type stateless)\n"
            << "(function bf3dpa_udiv          (Type) Type :cost 3 :type stateless)\n"
            << "(function bf3dpa_and           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_or            (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_xor           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_shl           (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_lshr          (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_ashr          (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_icmp          (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_load          (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3dpa_store         (Type) Type :cost 2 :type stateful :sigma 0.1)\n"
            << "(function bf3dpa_alloca        (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_getelementptr (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_cast          (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_thread_fence      (Type) Type :cost 2 :type stateless)\n"
            << "(function bf3dpa_thread_id         (Type) Type :cost 1 :type stateless)\n"
            << "(function bf3dpa_hash5tuple_apply  (Type) Type :cost 3 :type stateless)\n\n";

        for (auto &info : sorted) {
            std::string blkId   = "blk" + std::to_string(info.blockId);
            std::string implName = blkId + "_dpa";

            out << ";; ── Block " << info.blockId
                << " (DPA) ──────────────────────────────────\n";

            for (auto &[name, expr] : info.ops)
                out << "(let " << name << "\n    " << expr << ")\n";

            out << "(let " << implName << "_ops (vec-of";
            for (auto &[name, _] : info.ops)
                out << " " << name;
            out << "))\n";

            auto &preds = predecessors[info.blockId];
            out << "(implement " << implName << " DPA (";
            for (size_t i = 0; i < preds.size(); ++i) {
                if (i > 0) out << " ";
                out << "blk" << preds[i];
            }
            out << ") " << implName << "_ops)\n\n";
        }
        return success();
    }
};

// ============================================================================
// MLIR Pass Wrapper
// ============================================================================

struct VDPPToBF3DPAPass
    : public PassWrapper<VDPPToBF3DPAPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(VDPPToBF3DPAPass)

    VDPPToBF3DPAPass() = default;
    VDPPToBF3DPAPass(const VDPPToBF3DPAPass &other) : PassWrapper(other) {}

    StringRef getArgument()    const final { return "vdpp-to-bf3dpa"; }
    StringRef getDescription() const final {
        return "Fine-grained lowering: vDPP ops → bf3dpa ops "
               "(DialEgg-based, generates bf3dpa.mlir per block, "
               "dpa_handler.c, arm_handler.c)";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<vdpp::vDPPDialect>();
        registry.insert<bf3dpa::BF3DPADialect>();
        registry.insert<func::FuncDialect>();
    }

    void runOnOperation() override {
        auto *context = &getContext();

        llvm::outs() << "\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "  vDPP → bf3dpa Fine-Grained Lowering Pass (DialEgg)\n";
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

        BF3DPABlockGenerator generator(context, inputDir);
        if (failed(generator.generate())) {
            llvm::errs() << "❌ Failed to generate bf3dpa files\n";
            signalPassFailure();
            return;
        }

        llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "✅ bf3dpa lowering complete!\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    }
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

LogicalResult generateBF3DPACode(MLIRContext *ctx, llvm::StringRef inputDir) {
    BF3DPABlockGenerator gen(ctx, inputDir);
    return gen.generate();
}

std::unique_ptr<Pass> createVDPPToBF3DPAPass() {
    return std::make_unique<VDPPToBF3DPAPass>();
}

void registerVDPPToBF3DPAPass() {
    PassRegistration<VDPPToBF3DPAPass>();
}

} // namespace mlir
