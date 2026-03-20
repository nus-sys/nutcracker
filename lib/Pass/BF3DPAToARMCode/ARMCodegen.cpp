// ============================================================================
// File: lib/Pass/BF3DPAToARMCode/ARMCodegen.cpp
// bf3dpa → standard C codegen for ARM cores
//
// Generates one C function per vDSA block, callable from the BlueField-3
// ARM Cortex-A72 management cores (standard Linux userspace or kernel module
// context — full libc available).
//
//   void nc_blockN_arm(void *headers, void *metadata, void *std_meta,
//                      int *next_block);
//
// The generated C is portable standard C99/C11. Unlike DPA codegen, the ARM
// codegen may use stdint.h, string.h, and any POSIX APIs.
// ============================================================================

#include "Pass/BF3DPAToARMCodePass.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace mlir;

namespace {

static std::string mlirTypeToCarm(mlir::Type t) {
    if (auto ity = mlir::dyn_cast<mlir::IntegerType>(t)) {
        unsigned w = ity.getWidth();
        if (w == 1)  return "int";
        if (w <= 8)  return "uint8_t";
        if (w <= 16) return "uint16_t";
        if (w <= 32) return "uint32_t";
        return "uint64_t";
    }
    if (mlir::isa<vdpp::PointerType>(t)) return "void *";
    return "uint32_t";
}

// ============================================================================
// ARM C Emitter
// ============================================================================

class ARMCEmitter {
public:
    ARMCEmitter(llvm::raw_ostream &os, int blockId)
        : os(os), blockId(blockId) {}

    void emitFunction(func::FuncOp funcOp) {
        os << "/* Block " << blockId << ": " << funcOp.getName() << " (ARM) */\n";
        os << "int nc_block" << blockId
           << "_arm(void *headers, void *metadata, void *std_meta)\n{\n";

        auto args = funcOp.getArguments();
        if (args.size() >= 1) valueName[args[0]] = "headers";
        if (args.size() >= 2) valueName[args[1]] = "metadata";
        if (args.size() >= 3) valueName[args[2]] = "std_meta";

        // Build MLIR block → label index map.
        int bbIdx = 0;
        for (mlir::Block &block : funcOp.getFunctionBody())
            blockIndex[&block] = bbIdx++;

        // Emit each MLIR basic block.
        bbIdx = 0;
        for (mlir::Block &block : funcOp.getFunctionBody()) {
            if (bbIdx > 0)
                os << "  L_bb" << bbIdx << ":;\n";
            ++bbIdx;
            for (mlir::Operation &op : block)
                emitOp(op);
        }

        os << "}\n\n";
    }

private:
    llvm::raw_ostream &os;
    int blockId;
    llvm::DenseMap<mlir::Value, std::string> valueName;
    llvm::DenseMap<mlir::Block *, int> blockIndex;
    int tmpIdx = 0;

    std::string freshTmp() { return "t" + std::to_string(tmpIdx++); }

    std::string nameOf(mlir::Value v) {
        auto it = valueName.find(v);
        if (it != valueName.end()) return it->second;
        return "/*?*/";
    }

    void emitBinOp(mlir::Value result, mlir::Value lhs, mlir::Value rhs,
                   const char *op) {
        std::string tmp = freshTmp();
        os << "    " << mlirTypeToCarm(result.getType()) << " " << tmp
           << " = " << nameOf(lhs) << " " << op << " " << nameOf(rhs) << ";\n";
        valueName[result] = tmp;
    }

    void emitOp(mlir::Operation &op) {
        // ── bf3dpa arithmetic ─────────────────────────────────────────────
        if (auto o = mlir::dyn_cast<bf3dpa::AddOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "+"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::SubOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "-"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::MulOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "*"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::AndOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "&"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::OrOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "|"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::XorOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "^"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::ShlOp>(op))
            { emitBinOp(o.getResult(), o.getValue(), o.getAmount(), "<<"); return; }
        if (auto o = mlir::dyn_cast<bf3dpa::LShrOp>(op))
            { emitBinOp(o.getResult(), o.getValue(), o.getAmount(), ">>"); return; }

        // ── vdpp arithmetic fallback ──────────────────────────────────────
        if (auto o = mlir::dyn_cast<vdpp::AddOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "+"); return; }
        if (auto o = mlir::dyn_cast<vdpp::SubOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "-"); return; }
        if (auto o = mlir::dyn_cast<vdpp::AndOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "&"); return; }
        if (auto o = mlir::dyn_cast<vdpp::OrOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "|"); return; }
        if (auto o = mlir::dyn_cast<vdpp::XorOp>(op))
            { emitBinOp(o.getResult(), o.getLhs(), o.getRhs(), "^"); return; }

        // ── constants ─────────────────────────────────────────────────────
        if (auto o = mlir::dyn_cast<bf3dpa::ConstantOp>(op)) {
            std::string tmp = freshTmp();
            int64_t v = mlir::cast<mlir::IntegerAttr>(o.getValue()).getValue().getSExtValue();
            os << "    " << mlirTypeToCarm(o.getRes().getType()) << " "
               << tmp << " = " << v << ";\n";
            valueName[o.getRes()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<vdpp::ConstantOp>(op)) {
            std::string tmp = freshTmp();
            int64_t v = mlir::cast<mlir::IntegerAttr>(o.getValue()).getValue().getSExtValue();
            os << "    " << mlirTypeToCarm(o.getRes().getType()) << " "
               << tmp << " = " << v << ";\n";
            valueName[o.getRes()] = tmp;
            return;
        }

        // ── icmp ─────────────────────────────────────────────────────────
        auto emitIcmp = [&](mlir::Value result, mlir::Value lhs, mlir::Value rhs,
                             int64_t pred) {
            const char *cOp = "==";
            switch (pred) {
                case 0: cOp = "=="; break; case 1: cOp = "!="; break;
                case 2: cOp = "<";  break; case 3: cOp = "<="; break;
                case 4: cOp = ">";  break; case 5: cOp = ">="; break;
                default: cOp = "=="; break;
            }
            std::string tmp = freshTmp();
            os << "    int " << tmp << " = ("
               << nameOf(lhs) << " " << cOp << " " << nameOf(rhs) << ");\n";
            valueName[result] = tmp;
        };
        if (auto o = mlir::dyn_cast<bf3dpa::ICmpOp>(op))
            { emitIcmp(o.getResult(), o.getLhs(), o.getRhs(),
                       (int64_t)o.getPredicate()); return; }
        if (auto o = mlir::dyn_cast<vdpp::ICmpOp>(op))
            { emitIcmp(o.getResult(), o.getLhs(), o.getRhs(),
                       (int64_t)o.getPredicate()); return; }

        // ── memory ────────────────────────────────────────────────────────
        if (auto o = mlir::dyn_cast<bf3dpa::AllocaOp>(op)) {
            std::string tmp = freshTmp();
            auto ptrTy = mlir::cast<vdpp::PointerType>(o.getResult().getType());
            os << "    " << mlirTypeToCarm(ptrTy.getElementType())
               << " _" << tmp << "; void *" << tmp << " = &_" << tmp << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<vdpp::AllocaOp>(op)) {
            std::string tmp = freshTmp();
            auto ptrTy = mlir::cast<vdpp::PointerType>(o.getResult().getType());
            os << "    " << mlirTypeToCarm(ptrTy.getElementType())
               << " _" << tmp << "; void *" << tmp << " = &_" << tmp << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<bf3dpa::LoadOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToCarm(o.getResult().getType()) << " "
               << tmp << " = *(" << mlirTypeToCarm(o.getResult().getType())
               << " *)" << nameOf(o.getPtr()) << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<vdpp::LoadOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToCarm(o.getResult().getType()) << " "
               << tmp << " = *(" << mlirTypeToCarm(o.getResult().getType())
               << " *)" << nameOf(o.getPtr()) << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<bf3dpa::StoreOp>(op)) {
            os << "    *(" << mlirTypeToCarm(o.getValue().getType()) << " *)"
               << nameOf(o.getPtr()) << " = " << nameOf(o.getValue()) << ";\n";
            return;
        }
        if (auto o = mlir::dyn_cast<vdpp::StoreOp>(op)) {
            os << "    *(" << mlirTypeToCarm(o.getValue().getType()) << " *)"
               << nameOf(o.getPtr()) << " = " << nameOf(o.getValue()) << ";\n";
            return;
        }

        // ── GEP ──────────────────────────────────────────────────────────
        auto emitGep = [&](mlir::Value result, mlir::Value base,
                            mlir::ValueRange indices) {
            std::string tmp = freshTmp();
            std::string idxStr = indices.size() >= 2 ? nameOf(indices[1]) : "0";
            auto resPtrTy = mlir::cast<vdpp::PointerType>(result.getType());
            os << "    void *" << tmp << " = (uint8_t *)"
               << nameOf(base) << " + sizeof("
               << mlirTypeToCarm(resPtrTy.getElementType()) << ") * "
               << idxStr << ";\n";
            valueName[result] = tmp;
        };
        if (auto o = mlir::dyn_cast<bf3dpa::GetElementPtrOp>(op))
            { emitGep(o.getResult(), o.getBase(), o.getIndices()); return; }
        if (auto o = mlir::dyn_cast<vdpp::GetElementPtrOp>(op))
            { emitGep(o.getResult(), o.getBase(), o.getIndices()); return; }

        // ── cast ─────────────────────────────────────────────────────────
        if (auto o = mlir::dyn_cast<bf3dpa::CastOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToCarm(o.getResult().getType()) << " "
               << tmp << " = (" << mlirTypeToCarm(o.getResult().getType()) << ")"
               << nameOf(o.getValue()) << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }
        if (auto o = mlir::dyn_cast<vdpp::CastOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToCarm(o.getResult().getType()) << " "
               << tmp << " = (" << mlirTypeToCarm(o.getResult().getType()) << ")"
               << nameOf(o.getValue()) << ";\n";
            valueName[o.getResult()] = tmp;
            return;
        }

        // ── control flow ──────────────────────────────────────────────────
        if (auto condBr = mlir::dyn_cast<vdpp::CondBranchOp>(op)) {
            int trueIdx  = blockIndex.lookup(condBr.getTrueDest());
            int falseIdx = blockIndex.lookup(condBr.getFalseDest());
            os << "    if (" << nameOf(condBr.getCondition()) << ") "
               << "goto L_bb" << trueIdx << "; "
               << "else goto L_bb" << falseIdx << ";\n";
            return;
        }
        if (auto brOp = mlir::dyn_cast<vdpp::BranchOp>(op)) {
            int destIdx = blockIndex.lookup(brOp.getDest());
            os << "    goto L_bb" << destIdx << ";\n";
            return;
        }
        if (auto retOp = mlir::dyn_cast<vdpp::ReturnOp>(op)) {
            int32_t succ = -1;
            if (auto attr = retOp.getSuccessor())
                succ = *attr;
            os << "    return " << succ << ";\n";
            return;
        }
        if (mlir::isa<func::ReturnOp>(op)) {
            os << "    return -1;\n";
            return;
        }
        // DPA-specific ops are no-ops on ARM
        if (mlir::isa<bf3dpa::ThreadFenceOp, bf3dpa::ThreadIdOp,
                      bf3dpa::ProvisionedPtrOp, bf3dpa::PacketIndexOp>(op))
            return;

        os << "    /* unhandled: " << op.getName().getStringRef() << " */\n";
    }
};

} // namespace

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

LogicalResult emitARMFunction(int blockId, ModuleOp mod,
                              llvm::raw_ostream &os) {
    ARMCEmitter emitter(os, blockId);
    for (auto funcOp : mod.getOps<func::FuncOp>())
        emitter.emitFunction(funcOp);
    return success();
}

LogicalResult generateARMCode(MLIRContext *ctx, llvm::StringRef inputDir) {
    std::string outFile = (inputDir + "/arm_handler.c").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outFile, EC);
    if (EC) {
        llvm::errs() << "  ✗ Cannot open " << outFile << "\n";
        return failure();
    }

    out << "/* arm_handler.c — generated by nutcracker BF3DPAToARMCode pass */\n"
        << "/* Compile with standard ARM C compiler (gcc/clang) */\n\n"
        << "#include <stdint.h>\n"
        << "#include <string.h>\n"
        << "#include \"nc_types.h\"\n\n";

    std::vector<std::pair<int, std::string>> blocks;
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
         dir != end && !ec; dir.increment(ec)) {
        auto path = dir->path();
        if (!llvm::sys::fs::is_directory(path)) continue;
        std::string dirName = llvm::sys::path::filename(path).str();
        if (dirName.find("block") != 0) continue;
        int blockId = -1;
        llvm::StringRef(dirName).drop_front(5).getAsInteger(10, blockId);
        std::string bf3File = path + "/bf3dpa.mlir";
        if (!llvm::sys::fs::exists(bf3File)) continue;
        blocks.emplace_back(blockId, path);
    }
    llvm::sort(blocks, [](auto &a, auto &b) { return a.first < b.first; });

    if (blocks.empty()) {
        out << "/* No bf3dpa blocks found */\n";
        return success();
    }

    for (auto &[blockId, path] : blocks) {
        std::string bf3File = path + "/bf3dpa.mlir";
        auto mod = parseSourceFile<ModuleOp>(bf3File, ctx);
        if (!mod) {
            llvm::errs() << "  ✗ Cannot parse " << bf3File << "\n";
            return failure();
        }
        if (failed(emitARMFunction(blockId, *mod, out)))
            return failure();
    }

    // Emit a dispatch table.
    out << "/* ARM dispatch table */\n"
        << "typedef int (*nc_arm_block_fn)(void *, void *, void *);\n\n"
        << "static nc_arm_block_fn nc_arm_blocks[] = {\n";
    for (auto &[blockId, _] : blocks)
        out << "    nc_block" << blockId << "_arm,\n";
    out << "};\n\n"
        << "int nc_arm_num_blocks = " << blocks.size() << ";\n";

    return success();
}

} // namespace mlir
