// ============================================================================
// File: lib/Pass/BF3DPAToDPACode/DPACodegen.cpp
// bf3dpa → FlexIO restricted C codegen (DPA packet processing logic)
//
// Generates one C function per vDSA block intended to be called from inside
// process_packet() (see flexio_packet_processor_dev.c in the FlexIO SDK):
//
//   static int nc_blockN_process(void *headers, void *metadata,
//                                void *std_meta);
//
// Returns the next block ID to execute, or -1 when the pipeline is done.
//
// These are plain C helper functions with no FlexIO API calls — the caller
// (process_packet) handles CQE polling, RQ/SQ management, thread fences,
// and rescheduling.
//
// Restricted to: integer arithmetic, pointer GEP, load/store, compare,
// branch. No dynamic allocation, no stdlib, no float.
// ============================================================================

#include "Pass/BF3DPAToDPACodePass.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>

using namespace mlir;

namespace {

// ============================================================================
// C type helpers
// ============================================================================

static std::string mlirTypeToC(mlir::Type t) {
    if (auto ity = mlir::dyn_cast<mlir::IntegerType>(t)) {
        unsigned w = ity.getWidth();
        if (w == 1)  return "int";
        if (w <= 8)  return "uint8_t";
        if (w <= 16) return "uint16_t";
        if (w <= 32) return "uint32_t";
        return "uint64_t";
    }
    if (mlir::isa<vdpp::PointerType>(t)) return "void *";
    return "uint32_t"; // fallback
}

// ============================================================================
// DPA C emitter — plain C helpers called from inside process_packet()
// Each function returns int: the next block ID to execute, or -1 when done.
// ============================================================================

class DPACEmitter {
public:
    DPACEmitter(llvm::raw_ostream &os, int blockId)
        : os(os), blockId(blockId) {}

    void emitFunction(func::FuncOp funcOp) {
        os << "/* Block " << blockId << ": " << funcOp.getName() << " */\n";
        os << "static int nc_block" << blockId
           << "_process(void *headers, void *metadata, void *std_meta)\n{\n";

        // Map function arguments to caller-supplied pointers.
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

    std::string freshTmp() {
        return "t" + std::to_string(tmpIdx++);
    }

    std::string nameOf(mlir::Value v) {
        auto it = valueName.find(v);
        if (it != valueName.end()) return it->second;
        return "/*unknown*/";
    }

    void emitOp(mlir::Operation &op) {
        // ── bf3dpa arithmetic ─────────────────────────────────────────────
        if (auto addOp = mlir::dyn_cast<bf3dpa::AddOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(addOp.getType()) << " " << tmp
               << " = " << nameOf(addOp.getLhs())
               << " + " << nameOf(addOp.getRhs()) << ";\n";
            valueName[addOp.getResult()] = tmp;
            return;
        }
        if (auto subOp = mlir::dyn_cast<bf3dpa::SubOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(subOp.getType()) << " " << tmp
               << " = " << nameOf(subOp.getLhs())
               << " - " << nameOf(subOp.getRhs()) << ";\n";
            valueName[subOp.getResult()] = tmp;
            return;
        }
        if (auto mulOp = mlir::dyn_cast<bf3dpa::MulOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(mulOp.getType()) << " " << tmp
               << " = " << nameOf(mulOp.getLhs())
               << " * " << nameOf(mulOp.getRhs()) << ";\n";
            valueName[mulOp.getResult()] = tmp;
            return;
        }
        if (auto divOp = mlir::dyn_cast<bf3dpa::DivOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(divOp.getType()) << " " << tmp
               << " = (int32_t)" << nameOf(divOp.getLhs())
               << " / (int32_t)" << nameOf(divOp.getRhs()) << ";\n";
            valueName[divOp.getResult()] = tmp;
            return;
        }
        if (auto udivOp = mlir::dyn_cast<bf3dpa::UDivOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(udivOp.getType()) << " " << tmp
               << " = " << nameOf(udivOp.getLhs())
               << " / " << nameOf(udivOp.getRhs()) << ";\n";
            valueName[udivOp.getResult()] = tmp;
            return;
        }
        if (auto andOp = mlir::dyn_cast<bf3dpa::AndOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(andOp.getType()) << " " << tmp
               << " = " << nameOf(andOp.getLhs())
               << " & " << nameOf(andOp.getRhs()) << ";\n";
            valueName[andOp.getResult()] = tmp;
            return;
        }
        if (auto orOp = mlir::dyn_cast<bf3dpa::OrOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(orOp.getType()) << " " << tmp
               << " = " << nameOf(orOp.getLhs())
               << " | " << nameOf(orOp.getRhs()) << ";\n";
            valueName[orOp.getResult()] = tmp;
            return;
        }
        if (auto xorOp = mlir::dyn_cast<bf3dpa::XorOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(xorOp.getType()) << " " << tmp
               << " = " << nameOf(xorOp.getLhs())
               << " ^ " << nameOf(xorOp.getRhs()) << ";\n";
            valueName[xorOp.getResult()] = tmp;
            return;
        }
        if (auto shlOp = mlir::dyn_cast<bf3dpa::ShlOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(shlOp.getResult().getType()) << " " << tmp
               << " = " << nameOf(shlOp.getValue())
               << " << " << nameOf(shlOp.getAmount()) << ";\n";
            valueName[shlOp.getResult()] = tmp;
            return;
        }
        if (auto lshrOp = mlir::dyn_cast<bf3dpa::LShrOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(lshrOp.getResult().getType()) << " " << tmp
               << " = " << nameOf(lshrOp.getValue())
               << " >> " << nameOf(lshrOp.getAmount()) << ";\n";
            valueName[lshrOp.getResult()] = tmp;
            return;
        }
        if (auto ashrOp = mlir::dyn_cast<bf3dpa::AShrOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(ashrOp.getResult().getType()) << " " << tmp
               << " = (int32_t)" << nameOf(ashrOp.getValue())
               << " >> " << nameOf(ashrOp.getAmount()) << ";\n";
            valueName[ashrOp.getResult()] = tmp;
            return;
        }

        // ── bf3dpa constant ───────────────────────────────────────────────
        if (auto cstOp = mlir::dyn_cast<bf3dpa::ConstantOp>(op)) {
            std::string tmp = freshTmp();
            int64_t val = 0;
            if (auto ia = mlir::dyn_cast<mlir::IntegerAttr>(cstOp.getValue()))
                val = ia.getValue().getSExtValue();
            os << "    " << mlirTypeToC(cstOp.getRes().getType()) << " " << tmp
               << " = " << val << ";\n";
            valueName[cstOp.getRes()] = tmp;
            return;
        }

        // ── also handle vdpp.constant (may remain if DialEgg skipped it) ──
        if (auto cstOp = mlir::dyn_cast<vdpp::ConstantOp>(op)) {
            std::string tmp = freshTmp();
            int64_t val = 0;
            if (auto ia = mlir::dyn_cast<mlir::IntegerAttr>(cstOp.getValue()))
                val = ia.getValue().getSExtValue();
            os << "    " << mlirTypeToC(cstOp.getRes().getType()) << " " << tmp
               << " = " << val << ";\n";
            valueName[cstOp.getRes()] = tmp;
            return;
        }

        // ── icmp ──────────────────────────────────────────────────────────
        if (auto icmpOp = mlir::dyn_cast<bf3dpa::ICmpOp>(op)) {
            std::string tmp = freshTmp();
            auto pred = icmpOp.getPredicate();
            const char *cOp = "==";
            switch ((int64_t)pred) {
                case 0: cOp = "=="; break;
                case 1: cOp = "!="; break;
                case 2: cOp = "<";  break; // slt
                case 3: cOp = "<="; break; // sle
                case 4: cOp = ">";  break; // sgt
                case 5: cOp = ">="; break; // sge
                case 6: cOp = "<";  break; // ult
                case 7: cOp = "<="; break; // ule
                case 8: cOp = ">";  break; // ugt
                case 9: cOp = ">="; break; // uge
            }
            os << "    int " << tmp << " = ("
               << nameOf(icmpOp.getLhs()) << " " << cOp << " "
               << nameOf(icmpOp.getRhs()) << ");\n";
            valueName[icmpOp.getResult()] = tmp;
            return;
        }
        if (auto icmpOp = mlir::dyn_cast<vdpp::ICmpOp>(op)) {
            std::string tmp = freshTmp();
            auto pred = (int64_t)icmpOp.getPredicate();
            const char *cOp = "==";
            switch (pred) {
                case 0: cOp = "=="; break; case 1: cOp = "!="; break;
                case 2: cOp = "<";  break; case 3: cOp = "<="; break;
                case 4: cOp = ">";  break; case 5: cOp = ">="; break;
                case 6: cOp = "<";  break; case 7: cOp = "<="; break;
                case 8: cOp = ">";  break; case 9: cOp = ">="; break;
            }
            os << "    int " << tmp << " = ("
               << nameOf(icmpOp.getLhs()) << " " << cOp << " "
               << nameOf(icmpOp.getRhs()) << ");\n";
            valueName[icmpOp.getResult()] = tmp;
            return;
        }

        // ── memory ────────────────────────────────────────────────────────
        if (auto allocaOp = mlir::dyn_cast<bf3dpa::AllocaOp>(op)) {
            std::string tmp = freshTmp();
            auto ptrTy = mlir::cast<vdpp::PointerType>(allocaOp.getResult().getType());
            os << "    " << mlirTypeToC(ptrTy.getElementType()) << " _"
               << tmp << ";\n"
               << "    void *" << tmp << " = &_" << tmp << ";\n";
            valueName[allocaOp.getResult()] = tmp;
            return;
        }
        if (auto loadOp = mlir::dyn_cast<bf3dpa::LoadOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(loadOp.getResult().getType()) << " " << tmp
               << " = *(" << mlirTypeToC(loadOp.getResult().getType()) << " *)"
               << nameOf(loadOp.getPtr()) << ";\n";
            valueName[loadOp.getResult()] = tmp;
            return;
        }
        if (auto storeOp = mlir::dyn_cast<bf3dpa::StoreOp>(op)) {
            os << "    *(" << mlirTypeToC(storeOp.getValue().getType()) << " *)"
               << nameOf(storeOp.getPtr()) << " = "
               << nameOf(storeOp.getValue()) << ";\n";
            return;
        }
        // Also handle vdpp.load/store/alloca if DialEgg left them
        if (auto loadOp = mlir::dyn_cast<vdpp::LoadOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(loadOp.getResult().getType()) << " " << tmp
               << " = *(" << mlirTypeToC(loadOp.getResult().getType()) << " *)"
               << nameOf(loadOp.getPtr()) << ";\n";
            valueName[loadOp.getResult()] = tmp;
            return;
        }
        if (auto storeOp = mlir::dyn_cast<vdpp::StoreOp>(op)) {
            os << "    *(" << mlirTypeToC(storeOp.getValue().getType()) << " *)"
               << nameOf(storeOp.getPtr()) << " = "
               << nameOf(storeOp.getValue()) << ";\n";
            return;
        }
        if (auto allocaOp = mlir::dyn_cast<vdpp::AllocaOp>(op)) {
            std::string tmp = freshTmp();
            auto ptrTy = mlir::cast<vdpp::PointerType>(allocaOp.getResult().getType());
            os << "    " << mlirTypeToC(ptrTy.getElementType()) << " _"
               << tmp << ";\n"
               << "    void *" << tmp << " = &_" << tmp << ";\n";
            valueName[allocaOp.getResult()] = tmp;
            return;
        }

        // ── getelementptr ─────────────────────────────────────────────────
        // GEP into a struct: (base, 0, fieldIndex) → byte-offset pointer.
        if (auto gepOp = mlir::dyn_cast<bf3dpa::GetElementPtrOp>(op)) {
            std::string tmp = freshTmp();
            std::string idxStr = "0";
            auto indices = gepOp.getIndices();
            if (indices.size() >= 2)
                idxStr = nameOf(indices[1]);
            auto resPtrTy = mlir::cast<vdpp::PointerType>(gepOp.getResult().getType());
            os << "    /* GEP field " << idxStr << " */\n"
               << "    void *" << tmp << " = (uint8_t *)"
               << nameOf(gepOp.getBase()) << " + "
               << "sizeof(" << mlirTypeToC(resPtrTy.getElementType()) << ") * "
               << idxStr << ";\n";
            valueName[gepOp.getResult()] = tmp;
            return;
        }
        if (auto gepOp = mlir::dyn_cast<vdpp::GetElementPtrOp>(op)) {
            std::string tmp = freshTmp();
            auto indices = gepOp.getIndices();
            std::string idxStr = indices.size() >= 2 ? nameOf(indices[1]) : "0";
            auto resPtrTy = mlir::cast<vdpp::PointerType>(gepOp.getResult().getType());
            os << "    /* GEP field " << idxStr << " */\n"
               << "    void *" << tmp << " = (uint8_t *)"
               << nameOf(gepOp.getBase()) << " + "
               << "sizeof(" << mlirTypeToC(resPtrTy.getElementType()) << ") * "
               << idxStr << ";\n";
            valueName[gepOp.getResult()] = tmp;
            return;
        }

        // ── cast ─────────────────────────────────────────────────────────
        if (auto castOp = mlir::dyn_cast<bf3dpa::CastOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(castOp.getResult().getType()) << " " << tmp
               << " = (" << mlirTypeToC(castOp.getResult().getType()) << ")"
               << nameOf(castOp.getValue()) << ";\n";
            valueName[castOp.getResult()] = tmp;
            return;
        }
        if (auto castOp = mlir::dyn_cast<vdpp::CastOp>(op)) {
            std::string tmp = freshTmp();
            os << "    " << mlirTypeToC(castOp.getResult().getType()) << " " << tmp
               << " = (" << mlirTypeToC(castOp.getResult().getType()) << ")"
               << nameOf(castOp.getValue()) << ";\n";
            valueName[castOp.getResult()] = tmp;
            return;
        }

        // ── DPA intrinsics ────────────────────────────────────────────────
        if (auto fenceOp = mlir::dyn_cast<bf3dpa::ThreadFenceOp>(op)) {
            const char *dir = "__DPA_RW";
            switch ((int)fenceOp.getDir()) {
                case 0: dir = "__DPA_R";  break;
                case 1: dir = "__DPA_W";  break;
                case 2: dir = "__DPA_RW"; break;
            }
            os << "    __dpa_thread_fence(__DPA_MEMORY, " << dir
               << ", " << dir << ");\n";
            return;
        }
        if (auto tidOp = mlir::dyn_cast<bf3dpa::ThreadIdOp>(op)) {
            std::string tmp = freshTmp();
            // flexio_dev_get_thread_id is part of the DPA architecture intrinsics.
            os << "    uint32_t " << tmp
               << " = flexio_dev_get_thread_id(dtctx);\n";
            valueName[tidOp.getResult()] = tmp;
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

        // Unknown op: emit a comment.
        os << "    /* unhandled: " << op.getName().getStringRef() << " */\n";
    }
};

} // namespace

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

LogicalResult emitDPAEventHandler(int blockId, ModuleOp mod,
                                  llvm::raw_ostream &os) {
    DPACEmitter emitter(os, blockId);
    for (auto funcOp : mod.getOps<func::FuncOp>())
        emitter.emitFunction(funcOp);
    return success();
}

LogicalResult generateDPACode(MLIRContext *ctx, llvm::StringRef inputDir) {
    std::string outFile = (inputDir + "/dpa_handler.c").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outFile, EC);
    if (EC) {
        llvm::errs() << "  ✗ Cannot open " << outFile << "\n";
        return failure();
    }

    // File header — plain C, compiled with DPACC alongside the process_packet
    // wrapper that handles CQE polling, RQ/SQ management, and rescheduling.
    out << "/* dpa_handler.c — generated by nutcracker BF3DPAToDPACode pass */\n"
        << "/* Include this file from your process_packet translation unit,  */\n"
        << "/* or compile alongside it with DPACC (Clang-based DPA compiler). */\n\n"
        << "#include <stdint.h>\n"
        << "#include \"nc_types.h\"\n\n";

    // Collect and sort block directories.
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
        if (failed(emitDPAEventHandler(blockId, *mod, out)))
            return failure();
    }

    return success();
}

} // namespace mlir
