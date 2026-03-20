// ============================================================================
// File: lib/Pass/EmitHandlerCode/EmitHandlerCodePass.cpp
// --emit-handler-code pass
//
// Runs AFTER the mapper step (mapper.egg → mapping.txt).  Reads mapping.txt
// to find which blocks were assigned to DPA or ARM, then generates:
//
//   vdsa_output/nc_types.h      — packed C structs from p4hir type aliases
//   vdsa_output/dpa_handler.c   — DPA block functions (int return, next block)
//   vdsa_output/arm_handler.c   — ARM block functions (int return, next block)
//   vdsa_output/Makefile        — compiles arm_handler.c + test harness
//
// If no blocks map to DPA/ARM (e.g. all DRMT), the handler files are still
// emitted but contain only the file header — they still compile cleanly.
// ============================================================================

#include "Pass/EmitHandlerCodePass.h"
#include "Pass/BF3DPAToDPACodePass.h"
#include "Pass/BF3DPAToARMCodePass.h"
#include "Pass/BF3DRMTToDocaFlowPass.h"

#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace mlir;

namespace {

// ============================================================================
// Mapping parser
// ============================================================================

enum class HWTarget { UNKNOWN, DRMT, DPA, ARM };

static HWTarget parseTarget(llvm::StringRef s) {
    s = s.trim();
    if (s == "DRMT") return HWTarget::DRMT;
    if (s == "DPA")  return HWTarget::DPA;
    if (s == "ARM")  return HWTarget::ARM;
    return HWTarget::UNKNOWN;
}

// Parse the FIRST solution section from mapping.txt.
// Lines look like:  "   blk0_drmt          -> DRMT"
// Returns map from block ID → HWTarget.
static std::map<int, HWTarget>
parseMappingFile(llvm::StringRef inputDir) {
    std::map<int, HWTarget> result;
    std::string path = (inputDir + "/mapping.txt").str();
    auto bufOrErr = llvm::MemoryBuffer::getFile(path);
    if (!bufOrErr) return result;

    llvm::StringRef text = (*bufOrErr)->getBuffer();
    llvm::SmallVector<llvm::StringRef> lines;
    text.split(lines, '\n');

    bool inFirstSection = false;
    for (auto line : lines) {
        // Detect start of first solution section (box drawing char ┌ or ASCII [)
        if (line.contains("MINIMUM LATENCY") ||
            line.contains("MAXIMUM THROUGHPUT") ||
            line.contains("MINIMUM ENERGY")) {
            // Only use the first section found
            if (!result.empty()) break;
            inFirstSection = true;
            continue;
        }
        if (!inFirstSection) continue;

        // Lines with assignment: "   blkN_xxx   -> HW"
        auto arrowPos = line.find("->");
        if (arrowPos == llvm::StringRef::npos) continue;
        llvm::StringRef implName = line.take_front(arrowPos).trim();
        llvm::StringRef target   = line.drop_front(arrowPos + 2);

        // Strip box-drawing pipe chars and whitespace
        implName = implName.ltrim("│| \t");
        target   = target.trim().rtrim("│| \t│");

        // Parse block ID from implName: "blk{N}_{hw}"
        if (!implName.starts_with("blk")) continue;
        llvm::StringRef rest = implName.drop_front(3);
        int blockId = -1;
        size_t under = rest.find('_');
        if (under == llvm::StringRef::npos) continue;
        if (rest.take_front(under).getAsInteger(10, blockId)) continue;

        result[blockId] = parseTarget(target);
    }
    return result;
}

// ============================================================================
// Type header generation (from block0/p4hir.mlir)
// ============================================================================

static std::string p4hirTypeToC(llvm::StringRef ty) {
    if (ty == "!b1i" || ty == "!validity_bit") return "uint8_t";
    if (ty == "!b8i")  return "uint8_t";
    if (ty == "!b9i")  return "uint16_t";
    if (ty == "!b16i") return "uint16_t";
    if (ty == "!b32i") return "uint32_t";
    if (ty == "!b64i") return "uint64_t";
    if (ty.starts_with("!")) return ty.drop_front(1).str();
    return "uint32_t";
}

static std::pair<std::string, std::string> parseField(llvm::StringRef field) {
    field = field.trim();
    auto colonPos = field.find(':');
    if (colonPos == llvm::StringRef::npos) return {};
    std::string name  = field.take_front(colonPos).trim().str();
    std::string ctype = p4hirTypeToC(field.drop_front(colonPos + 1).trim());
    return {name, ctype};
}

static LogicalResult generateTypeHeader(llvm::StringRef inputDir) {
    std::string p4hirFile = (inputDir + "/block0/p4hir.mlir").str();
    auto bufOrErr = llvm::MemoryBuffer::getFile(p4hirFile);
    if (!bufOrErr) {
        llvm::errs() << "  ✗ Cannot read " << p4hirFile << "\n";
        return failure();
    }

    struct StructDef {
        std::string alias;
        std::vector<std::pair<std::string, std::string>> fields;
    };
    std::vector<StructDef> defs;

    llvm::SmallVector<llvm::StringRef> lines;
    (*bufOrErr)->getBuffer().split(lines, '\n');
    for (auto line : lines) {
        line = line.trim();
        if (!line.starts_with("!")) continue;
        auto eqPos = line.find(" = !p4hir.");
        if (eqPos == llvm::StringRef::npos) continue;
        std::string alias = line.take_front(eqPos).drop_front(1).str();
        llvm::StringRef rest = line.drop_front(eqPos + 10);
        if (!rest.starts_with("header<") && !rest.starts_with("struct<")) continue;
        auto ltPos = rest.find('<');
        auto gtPos = rest.rfind('>');
        if (ltPos == llvm::StringRef::npos || gtPos <= ltPos) continue;
        llvm::StringRef content = rest.slice(ltPos + 1, gtPos);

        llvm::SmallVector<llvm::StringRef> parts;
        content.split(parts, ',');

        StructDef def;
        def.alias = alias;
        bool first = true;
        for (auto part : parts) {
            if (first) { first = false; continue; }
            auto [name, ctype] = parseField(part);
            if (!name.empty())
                def.fields.push_back({name, ctype});
        }
        defs.push_back(def);
    }

    std::string outFile = (inputDir + "/nc_types.h").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outFile, EC);
    if (EC) { llvm::errs() << "  ✗ Cannot open " << outFile << "\n"; return failure(); }

    out << "/* nc_types.h — generated by nutcracker (do not edit) */\n"
        << "#pragma once\n"
        << "#include <stdint.h>\n\n";
    for (auto &def : defs) {
        out << "typedef struct __attribute__((packed)) {\n";
        for (auto &[fname, ctype] : def.fields)
            out << "    " << ctype << " " << fname << ";\n";
        out << "} " << def.alias << ";\n\n";
    }
    return success();
}

// ============================================================================
// Makefile generation
// ============================================================================

static LogicalResult generateMakefile(llvm::StringRef inputDir,
                                       const std::vector<int> &armBlocks,
                                       const std::vector<int> &dpaBlocks) {
    std::string outFile = (inputDir + "/Makefile").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outFile, EC);
    if (EC) { llvm::errs() << "  ✗ Cannot open " << outFile << "\n"; return failure(); }

    out << "# Makefile — generated by nutcracker (do not edit)\n"
        << "CC      ?= gcc\n"
        << "CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -I.\n\n";

    // ARM test binary (compilable on host / BF3 ARM cores)
    out << "all: arm_test\n\n"
        << "arm_test: arm_handler.c arm_test_main.c nc_types.h\n"
        << "\t$(CC) $(CFLAGS) arm_handler.c arm_test_main.c -o arm_test\n\n";

    if (!dpaBlocks.empty()) {
        out << "# DPA code must be compiled with DPACC (Clang-based DPA compiler)\n"
            << "# dpa_handler.o: dpa_handler.c nc_types.h\n"
            << "#\tdpacc dpa_handler.c -o dpa_handler.o\n\n";
    }

    out << "clean:\n\trm -f arm_test arm_test_main.c\n\n"
        << ".PHONY: all clean\n";

    // Generate test main
    std::string testFile = (inputDir + "/arm_test_main.c").str();
    llvm::raw_fd_ostream test(testFile, EC);
    if (EC) { llvm::errs() << "  ✗ Cannot open " << testFile << "\n"; return failure(); }

    test << "/* arm_test_main.c — generated by nutcracker (do not edit) */\n"
         << "#include <stdio.h>\n"
         << "#include <string.h>\n"
         << "#include \"nc_types.h\"\n"
         << "#include \"arm_handler.c\"\n\n"
         << "int main(void) {\n"
         << "    main_headers_t  hdr;  memset(&hdr,  0, sizeof hdr);\n"
         << "    main_metadata_t meta; memset(&meta, 0, sizeof meta);\n"
         << "    nc_standard_metadata_t std_meta; memset(&std_meta, 0, sizeof std_meta);\n\n"
         << "    int block = 0;\n"
         << "    while (block >= 0) {\n"
         << "        switch (block) {\n";
    for (int id : armBlocks)
        test << "        case " << id << ": block = nc_block" << id
             << "_arm(&hdr, &meta, &std_meta); break;\n";
    test << "        default: block = -1; break;\n"
         << "        }\n"
         << "    }\n"
         << "    printf(\"dispatch done\\n\");\n"
         << "    return 0;\n"
         << "}\n";
    return success();
}

// ============================================================================
// Pass implementation
// ============================================================================

class EmitHandlerCodePass
    : public PassWrapper<EmitHandlerCodePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(EmitHandlerCodePass)

    llvm::StringRef getArgument() const override { return "emit-handler-code"; }
    llvm::StringRef getName() const override { return "emit-handler-code"; }
    llvm::StringRef getDescription() const override {
        return "Emit nc_types.h, dpa_handler.c, arm_handler.c from mapping";
    }

    void getDependentDialects(mlir::DialectRegistry &registry) const override {
        registry.insert<mlir::vdpp::vDPPDialect>();
        registry.insert<mlir::bf3dpa::BF3DPADialect>();
        registry.insert<mlir::bf3drmt::BF3DRMTDialect>();
        registry.insert<mlir::func::FuncDialect>();
    }

    void runOnOperation() override {
        std::string inputDir = "vdsa_output";

        llvm::outs() << "\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "  Emit Handler Code Pass (mapping-aware)\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

        // Step 1: Parse mapping
        auto mapping = parseMappingFile(inputDir);
        if (mapping.empty()) {
            llvm::outs() << "  ⚠  No mapping found in " << inputDir
                         << "/mapping.txt — emitting all blocks\n";
            // Fallback: emit all blocks as ARM
            std::error_code ec;
            for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
                 dir != end && !ec; dir.increment(ec)) {
                std::string dirName = llvm::sys::path::filename(dir->path()).str();
                if (dirName.find("block") != 0) continue;
                int id = -1;
                llvm::StringRef(dirName).drop_front(5).getAsInteger(10, id);
                if (id >= 0) mapping[id] = HWTarget::ARM;
            }
        }

        std::vector<int> dpaBlocks, armBlocks, drmtBlocks;
        for (auto &[id, hw] : mapping) {
            if (hw == HWTarget::DPA)  dpaBlocks.push_back(id);
            if (hw == HWTarget::ARM)  armBlocks.push_back(id);
            if (hw == HWTarget::DRMT) drmtBlocks.push_back(id);
        }

        if (dpaBlocks.empty() && armBlocks.empty()) {
            llvm::outs() << "  No DPA or ARM blocks in mapping — skipping handler codegen.\n"
                         << "  All blocks run on DRMT; see doca_flow_pipeline.c.\n";
        } else {
            // Step 2: Type header (only needed when DPA/ARM handlers are generated)
            llvm::outs() << "  Generating nc_types.h ...\n";
            if (failed(generateTypeHeader(inputDir))) { signalPassFailure(); return; }
            llvm::outs() << "  ✓ Generated nc_types.h\n";

            // Step 3: DPA handler
            if (!dpaBlocks.empty()) {
                llvm::outs() << "  Generating dpa_handler.c ("
                             << dpaBlocks.size() << " blocks) ...\n";
                if (failed(emitDPAHandler(inputDir, dpaBlocks))) {
                    signalPassFailure(); return;
                }
                llvm::outs() << "  ✓ Generated dpa_handler.c\n";
            }

            // Step 4: ARM handler + Makefile
            if (!armBlocks.empty()) {
                llvm::outs() << "  Generating arm_handler.c ("
                             << armBlocks.size() << " blocks) ...\n";
                if (failed(emitARMHandler(inputDir, armBlocks))) {
                    signalPassFailure(); return;
                }
                llvm::outs() << "  ✓ Generated arm_handler.c\n";

                llvm::outs() << "  Generating Makefile + arm_test_main.c ...\n";
                if (failed(generateMakefile(inputDir, armBlocks, dpaBlocks))) {
                    signalPassFailure(); return;
                }
                llvm::outs() << "  ✓ Generated Makefile\n";
            }

            llvm::outs() << "\n  DPA blocks: ";
            for (int id : dpaBlocks) llvm::outs() << id << " ";
            llvm::outs() << "\n  ARM blocks: ";
            for (int id : armBlocks) llvm::outs() << id << " ";
            llvm::outs() << "\n";
        }

        // Step 5: Always generate the deploy/ directory with the full mapping
        llvm::outs() << "\n  Generating deploy/ directory ...\n";
        if (failed(generateDeployDir(inputDir, mapping, dpaBlocks, armBlocks,
                                     drmtBlocks))) {
            signalPassFailure(); return;
        }
        llvm::outs() << "  ✓ Generated deploy/\n";

        llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        llvm::outs() << "✅ Handler code emission complete!\n";
        llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    }

private:
    MLIRContext *getMLIRContext() { return &getContext(); }

    // =========================================================================
    // computeArmEntrypoints — find ARM blocks that receive packets from DRMT
    // (i.e. they appear as NextOp successors in DRMT blocks' bf3drmt.mlir).
    // These are the blocks that need a dedicated RSS queue.
    // Chained ARM blocks (ARM→ARM) are called directly and need no queue.
    // =========================================================================
    std::vector<int> computeArmEntrypoints(llvm::StringRef inputDir,
                                            const std::vector<int> &drmtBlocks,
                                            const std::set<int> &armBlockSet) {
        std::set<int> found;
        std::vector<int> entrypoints;
        auto *ctx = getMLIRContext();
        for (int drmtId : drmtBlocks) {
            std::string bf3File = (inputDir + "/block" + std::to_string(drmtId)
                                   + "/bf3drmt.mlir").str();
            if (!llvm::sys::fs::exists(bf3File)) continue;
            auto mod = parseSourceFile<ModuleOp>(bf3File, ctx);
            if (!mod) continue;
            mod->walk([&](bf3drmt::NextOp nextOp) {
                int s = (int)nextOp.getSuccessor();
                if (s >= 0 && armBlockSet.count(s) && !found.count(s)) {
                    entrypoints.push_back(s);
                    found.insert(s);
                }
            });
        }
        llvm::sort(entrypoints);
        return entrypoints;
    }

    // =========================================================================
    // generateDeployDir — produce vdsa_output/deploy/ runtime integration files
    // =========================================================================
    LogicalResult generateDeployDir(llvm::StringRef inputDir,
                                     const std::map<int, HWTarget> &mapping,
                                     const std::vector<int> &dpaBlocks,
                                     const std::vector<int> &armBlocks,
                                     const std::vector<int> &drmtBlocks) {
        std::string deployDir = (inputDir + "/deploy").str();

        // Create deploy/ directory
        if (auto ec = llvm::sys::fs::create_directories(deployDir)) {
            llvm::errs() << "  ✗ Cannot create directory " << deployDir
                         << ": " << ec.message() << "\n";
            return failure();
        }

        int numBlocks = (int)mapping.size();
        if (numBlocks == 0) numBlocks = 1;

        // Determine highest block ID to size arrays correctly
        int maxBlockId = -1;
        for (auto &[id, hw] : mapping)
            if (id > maxBlockId) maxBlockId = id;
        numBlocks = maxBlockId + 1;

        // Compute ARM entrypoints (DRMT→ARM transitions) and their queue assignments
        std::set<int> armBlockSet(armBlocks.begin(), armBlocks.end());
        std::vector<int> armEntrypoints =
            computeArmEntrypoints(inputDir, drmtBlocks, armBlockSet);
        // Fallback: if no DRMT blocks forward to ARM, treat all ARM blocks as entrypoints
        if (armEntrypoints.empty() && !armBlocks.empty())
            armEntrypoints = armBlocks;

        // armQueueMap: ARM entrypoint block_id → queue index (0, 1, 2, ...)
        std::map<int, int> armQueueMap;
        for (int i = 0; i < (int)armEntrypoints.size(); i++)
            armQueueMap[armEntrypoints[i]] = i;

        // blockHwMap for codegen: block_id → 0=DRMT, 1=DPA, 2=ARM
        std::map<int, int> blockHwMap;
        for (auto &[id, hw] : mapping) {
            switch (hw) {
            case HWTarget::DPA:  blockHwMap[id] = 1; break;
            case HWTarget::ARM:  blockHwMap[id] = 2; break;
            default:             blockHwMap[id] = 0; break;
            }
        }

        // ── 1. nc_pipeline.h ─────────────────────────────────────────────────
        {
            std::string outFile = deployDir + "/nc_pipeline.h";
            std::error_code EC;
            llvm::raw_fd_ostream out(outFile, EC);
            if (EC) {
                llvm::errs() << "  ✗ Cannot open " << outFile << "\n";
                return failure();
            }

            out << "/* nc_pipeline.h — generated by nutcracker (do not edit)\n"
                << " * Block → hardware mapping for this compilation.\n"
                << " * Included by nc_runtime.h; do NOT define nc_hw_target_t here.\n"
                << " */\n"
                << "#pragma once\n\n"
                << "#define NC_NUM_BLOCKS  " << numBlocks << "\n"
                << "#define NC_FIRST_BLOCK 0\n\n"
                << "/* nc_hw_target_t is defined in nc_runtime.h */\n\n"
                << "static const nc_hw_target_t nc_block_hw[NC_NUM_BLOCKS] = {\n";

            for (int i = 0; i < numBlocks; i++) {
                auto it = mapping.find(i);
                std::string hwStr = "NC_HW_DRMT"; // default
                if (it != mapping.end()) {
                    switch (it->second) {
                    case HWTarget::DPA:  hwStr = "NC_HW_DPA";  break;
                    case HWTarget::ARM:  hwStr = "NC_HW_ARM";  break;
                    case HWTarget::DRMT: hwStr = "NC_HW_DRMT"; break;
                    default: break;
                    }
                }
                out << "    " << hwStr << ",  /* block " << i << " */\n";
            }
            out << "};\n\n";

            // ARM queue mapping: one RSS queue per DRMT→ARM entrypoint
            out << "/* ARM RSS queues: one per DRMT→ARM entrypoint block */\n"
                << "#define NC_NUM_ARM_QUEUES " << armEntrypoints.size() << "\n";
            if (!armEntrypoints.empty()) {
                out << "\n/* queue index → ARM entrypoint block ID */\n"
                    << "static const int nc_arm_queue_to_block[NC_NUM_ARM_QUEUES] = {\n";
                for (int i = 0; i < (int)armEntrypoints.size(); i++)
                    out << "    " << armEntrypoints[i]
                        << ",  /* queue " << i << " → block " << armEntrypoints[i]
                        << " */\n";
                out << "};\n";
            }
        }

        // ── 2. Copy nc_types.h from vdsa_output/ ─────────────────────────────
        {
            std::string srcFile  = (inputDir + "/nc_types.h").str();
            std::string destFile = deployDir + "/nc_types.h";
            auto bufOrErr = llvm::MemoryBuffer::getFile(srcFile);
            if (bufOrErr) {
                std::error_code EC;
                llvm::raw_fd_ostream out(destFile, EC);
                if (!EC)
                    out << (*bufOrErr)->getBuffer();
            } else {
                // If nc_types.h doesn't exist yet (all-DRMT pipeline), generate
                // a minimal stub so deploy/ still compiles.
                std::error_code EC;
                llvm::raw_fd_ostream out(destFile, EC);
                if (!EC) {
                    out << "/* nc_types.h — stub (no DPA/ARM blocks) */\n"
                        << "#pragma once\n"
                        << "#include <stdint.h>\n\n"
                        << "typedef struct __attribute__((packed)) {\n"
                        << "    uint8_t _placeholder;\n"
                        << "} main_headers_t;\n\n"
                        << "typedef struct __attribute__((packed)) {\n"
                        << "    uint8_t _placeholder;\n"
                        << "} main_metadata_t;\n\n"
                        << "typedef struct __attribute__((packed)) {\n"
                        << "    uint32_t ingress_port;\n"
                        << "    uint32_t egress_port;\n"
                        << "    uint32_t drop;\n"
                        << "} nc_standard_metadata_t;\n";
                }
            }
        }

        // ── 3. arm_handler.c — copy from vdsa_output/ if ARM blocks exist ───
        if (!armBlocks.empty()) {
            std::string srcFile  = (inputDir + "/arm_handler.c").str();
            std::string destFile = deployDir + "/arm_handler.c";
            auto bufOrErr = llvm::MemoryBuffer::getFile(srcFile);
            if (bufOrErr) {
                std::error_code EC;
                llvm::raw_fd_ostream out(destFile, EC);
                if (!EC)
                    out << (*bufOrErr)->getBuffer();
            } else {
                llvm::errs() << "  ✗ Cannot read " << srcFile
                             << " (needed for ARM blocks)\n";
                return failure();
            }
        }

        // ── 4. dpa_handler.c — copy from vdsa_output/ if DPA blocks exist ───
        if (!dpaBlocks.empty()) {
            std::string srcFile  = (inputDir + "/dpa_handler.c").str();
            std::string destFile = deployDir + "/dpa_handler.c";
            auto bufOrErr = llvm::MemoryBuffer::getFile(srcFile);
            if (bufOrErr) {
                std::error_code EC;
                llvm::raw_fd_ostream out(destFile, EC);
                if (!EC)
                    out << (*bufOrErr)->getBuffer();
            } else {
                llvm::errs() << "  ✗ Cannot read " << srcFile
                             << " (needed for DPA blocks)\n";
                return failure();
            }
        }

        // ── 5. dpa_dev_entry.c — DPA device-side event handler ───────────────
        if (!dpaBlocks.empty()) {
            std::string outFile = deployDir + "/dpa_dev_entry.c";
            std::error_code EC;
            llvm::raw_fd_ostream out(outFile, EC);
            if (EC) {
                llvm::errs() << "  ✗ Cannot open " << outFile << "\n";
                return failure();
            }

            out << "/* dpa_dev_entry.c — DPA device-side entry point.\n"
                << " * Generated by nutcracker (do not edit).\n"
                << " * Compiled with DPACC alongside dpa_handler.c.\n"
                << " * The event handler receives packets, calls nc_blockN_process(),\n"
                << " * and writes the next-block ID into the packet metadata.\n"
                << " */\n"
                << "#include <libflexio-dev/flexio_dev.h>\n"
                << "#include <libflexio-dev/flexio_dev_queue_access.h>\n"
                << "#include <dpaintrin.h>\n"
                << "#include <stdint.h>\n"
                << "#include \"nc_types.h\"\n\n"
                << "/* Forward declarations of generated block functions (from dpa_handler.c) */\n";

            for (int id : dpaBlocks)
                out << "int nc_block" << id
                    << "_process(void *headers, void *metadata, void *std_meta);\n";

            out << "\n"
                << "/* Transfer structure matching nc_flexio_host.c */\n"
                << "typedef struct {\n"
                << "    uint32_t rq_cq_num;\n"
                << "    uint32_t rq_num;\n"
                << "    uint32_t sq_cq_num;\n"
                << "    uint32_t sq_num;\n"
                << "    uint32_t rq_mkey_id;\n"
                << "    uint32_t sq_mkey_id;\n"
                << "} nc_dpa_app_data_t;\n\n"
                << "flexio_dev_event_handler_t nc_dpa_event_handler;\n\n"
                << "__dpa_global__ void nc_dpa_event_handler(uint64_t thread_arg) {\n"
                << "    struct flexio_dev_thread_ctx *dtctx;\n"
                << "    flexio_dev_get_thread_ctx(&dtctx);\n\n"
                << "    nc_dpa_app_data_t *app_data = (nc_dpa_app_data_t *)thread_arg;\n\n"
                << "    /* Process incoming packet */\n"
                << "    struct flexio_dev_cqe64 *cqe;\n"
                << "    struct flexio_dev_wqe_rcv_data_seg *rwqe;\n"
                << "    uint32_t cq_idx = 0;  /* Track CQ consumer index */\n\n"
                << "    cqe = flexio_dev_cqe_get(app_data->rq_cq_num, cq_idx);\n"
                << "    if (!flexio_dev_cqe_is_hw_owned(cqe)) {\n"
                << "        /* Valid CQE — get packet data */\n"
                << "        rwqe = flexio_dev_rwqe_get(app_data->rq_num,\n"
                << "                                    flexio_dev_cqe_get_wqe_counter(cqe));\n"
                << "        uint8_t *pkt = (uint8_t *)flexio_dev_rwqe_get_addr(rwqe);\n"
                << "        uint32_t pkt_len = flexio_dev_cqe_get_byte_cnt(cqe);\n\n"
                << "        /* Block ID is stored as a 4-byte tag appended after packet data */\n"
                << "        uint32_t block_id = *(uint32_t *)(pkt + pkt_len);\n\n"
                << "        /* Unpack packet into struct pointers */\n"
                << "        void *hdr      = pkt;\n"
                << "        void *meta     = pkt + sizeof(main_headers_t);\n"
                << "        void *std_meta = pkt + sizeof(main_headers_t)"
                << " + sizeof(main_metadata_t);\n\n"
                << "        /* Dispatch to the correct DPA block */\n"
                << "        int next_block = -1;\n"
                << "        switch (block_id) {\n";

            for (int id : dpaBlocks)
                out << "        case " << id << ":\n"
                    << "            next_block = nc_block" << id
                    << "_process(hdr, meta, std_meta);\n"
                    << "            break;\n";

            out << "        default: break;\n"
                << "        }\n\n"
                << "        /* Write next_block into metadata tag */\n"
                << "        *(uint32_t *)(pkt + pkt_len) = (uint32_t)next_block;\n"
                << "        /* TODO: post SQ WQE to send packet back to host with next_block"
                << " in meta */\n"
                << "    }\n\n"
                << "    flexio_dev_thread_reschedule();\n"
                << "}\n";
        }

        // ── 5b. deploy/nc_dpa_stubs.c (no-DPA case) ─────────────────────────
        // When there are no DPA blocks, the runtime still references nc_dpa_app
        // and nc_dpa_event_handler (DPACC-generated symbols).  Provide stubs so
        // the binary links without a DPA binary.
        if (dpaBlocks.empty()) {
            std::string outFile = deployDir + "/nc_dpa_stubs.c";
            std::error_code EC;
            llvm::raw_fd_ostream out(outFile, EC);
            if (!EC) {
                out << "/* nc_dpa_stubs.c — generated by nutcracker (do not edit)\n"
                    << " * Stub symbols for nc_dpa_app and nc_dpa_event_handler.\n"
                    << " * Included when no DPA blocks are in the mapping.\n"
                    << " */\n"
                    << "#include <libflexio/flexio.h>\n\n"
                    << "/* Stub: no DPA blocks in this mapping */\n"
                    << "struct flexio_app *nc_dpa_app = NULL;\n"
                    << "void nc_dpa_event_handler(void) {}\n";
            }
        }

        // ── 6. deploy/meson.build ─────────────────────────────────────────────
        {
            // Resolve absolute paths, then compute runtime path relative to deployDir.
            // Meson resolves source paths relative to the meson.build file's directory.
            std::string runtimeAbsPath;
            if (const char *root = std::getenv("NUTCRACKER_ROOT")) {
                runtimeAbsPath = std::string(root) + "/runtime";
            } else {
                llvm::SmallString<256> rel(inputDir);
                llvm::sys::path::append(rel, "..", "..", "runtime");
                llvm::SmallString<256> resolved;
                llvm::sys::fs::real_path(rel, resolved);
                runtimeAbsPath = resolved.str().str();
            }

            // Compute path to runtime relative to deployDir
            std::string runtimeRelPath =
                std::filesystem::relative(runtimeAbsPath, deployDir).generic_string();

            std::string outFile = deployDir + "/meson.build";
            std::error_code EC;
            llvm::raw_fd_ostream out(outFile, EC);
            if (EC) {
                llvm::errs() << "  ✗ Cannot open " << outFile << "\n";
                return failure();
            }

            out << "# deploy/meson.build — generated by nutcracker (do not edit)\n"
                << "# Runtime sources referenced by relative path (" << runtimeRelPath << ")\n"
                << "# Run: meson setup _build && ninja -C _build\n"
                << "project('nc_deploy', 'c',\n"
                << "    default_options: ['buildtype=debug'],\n"
                << "    meson_version: '>= 0.61.2'\n"
                << ")\n\n";

            // External library dependencies
            out << "nc_deps = [\n"
                << "  dependency('doca-common'),\n"
                << "  dependency('doca-flow'),\n"
                << "  dependency('doca-argp'),\n"
                << "  dependency('libdpdk'),\n"
                << "  dependency('libflexio'),\n"
                << "  dependency('libibverbs'),\n"
                << "  dependency('libmlx5'),\n"
                << "]\n\n";

            // Sources: generated files + runtime .c files via relative path.
            // deploy/ is first in include_directories so the generated nc_pipeline.h
            // and nc_types.h shadow the template versions in runtime/include/.
            out << "nc_sources = [\n"
                << "  'doca_flow_pipeline.c',\n";
            if (!armBlocks.empty())
                out << "  'arm_handler.c',\n";
            if (dpaBlocks.empty())
                out << "  'nc_dpa_stubs.c',\n";
            out << "  '" << runtimeRelPath << "/nc_runtime_main.c',\n"
                << "  '" << runtimeRelPath << "/nc_doca_flow.c',\n"
                << "  '" << runtimeRelPath << "/nc_flexio_host.c',\n"
                << "  '" << runtimeRelPath << "/nc_arm_worker.c',\n"
                << "]\n\n";

            if (!dpaBlocks.empty()) {
                out << "# DPA device-side code must be compiled separately with DPACC:\n"
                    << "#   dpacc dpa_handler.c dpa_dev_entry.c -o nc_dpa_app\n"
                    << "# Link the resulting object alongside this binary.\n\n";
            }

            out << "executable('nc_pipeline',\n"
                << "  nc_sources,\n"
                << "  dependencies: nc_deps,\n"
                << "  include_directories: include_directories(\n"
                << "    '.',\n"
                << "    '" << runtimeRelPath << "/include',\n"
                << "  ),\n"
                << "  c_args: ['-DDOCA_ALLOW_EXPERIMENTAL_API', '-Wno-missing-braces'],\n"
                << "  install: false,\n"
                << ")\n";
        }

        // ── 7. deploy/doca_flow_pipeline.c ───────────────────────────────────
        // Re-generate with the full hardware mapping so DRMT→ARM/DPA forwards
        // emit the correct RSS queue indices instead of DOCA_FLOW_FWD_PIPE.
        {
            llvm::outs() << "  Generating deploy/doca_flow_pipeline.c"
                         << " (with HW mapping) ...\n";
            if (failed(generateDocaFlowCode(getMLIRContext(), inputDir,
                                            deployDir, blockHwMap, armQueueMap))) {
                llvm::errs() << "  ✗ DOCA Flow codegen failed\n";
                return failure();
            }
        }

        llvm::outs() << "    deploy/nc_pipeline.h  (" << numBlocks << " blocks, "
                     << armEntrypoints.size() << " ARM queues)\n"
                     << "    deploy/nc_types.h\n"
                     << "    deploy/doca_flow_pipeline.c\n";
        if (!armBlocks.empty()) llvm::outs() << "    deploy/arm_handler.c\n";
        if (!dpaBlocks.empty()) {
            llvm::outs() << "    deploy/dpa_handler.c\n"
                         << "    deploy/dpa_dev_entry.c\n";
        } else {
            llvm::outs() << "    deploy/nc_dpa_stubs.c\n";
        }
        llvm::outs() << "    deploy/meson.build\n";

        return success();
    }

    LogicalResult emitDPAHandler(llvm::StringRef inputDir,
                                  const std::vector<int> &blocks) {
        std::string outFile = (inputDir + "/dpa_handler.c").str();
        std::error_code EC;
        llvm::raw_fd_ostream out(outFile, EC);
        if (EC) { llvm::errs() << "  ✗ Cannot open " << outFile << "\n"; return failure(); }

        out << "/* dpa_handler.c — generated by nutcracker (do not edit) */\n"
            << "/* Compile with DPACC alongside your process_packet() wrapper. */\n"
            << "/* Each function returns the next block ID, or -1 when done.   */\n\n"
            << "#include <stdint.h>\n"
            << "#include \"nc_types.h\"\n\n";

        if (blocks.empty()) {
            out << "/* No blocks assigned to DPA in the selected mapping. */\n";
            return success();
        }

        auto *ctx = getMLIRContext();
        for (int blockId : blocks) {
            std::string bf3File = (inputDir + "/block" + std::to_string(blockId)
                                   + "/bf3dpa.mlir").str();
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

    LogicalResult emitARMHandler(llvm::StringRef inputDir,
                                  const std::vector<int> &blocks) {
        std::string outFile = (inputDir + "/arm_handler.c").str();
        std::error_code EC;
        llvm::raw_fd_ostream out(outFile, EC);
        if (EC) { llvm::errs() << "  ✗ Cannot open " << outFile << "\n"; return failure(); }

        out << "/* arm_handler.c — generated by nutcracker (do not edit) */\n"
            << "/* Compile with gcc/clang on BlueField-3 ARM cores.         */\n"
            << "/* Each function returns the next block ID, or -1 when done. */\n\n"
            << "#include <stdint.h>\n"
            << "#include <string.h>\n"
            << "#include \"nc_types.h\"\n\n";

        if (blocks.empty()) {
            out << "/* No blocks assigned to ARM in the selected mapping. */\n";
            return success();
        }

        auto *ctx = getMLIRContext();
        for (int blockId : blocks) {
            std::string bf3File = (inputDir + "/block" + std::to_string(blockId)
                                   + "/bf3dpa.mlir").str();
            auto mod = parseSourceFile<ModuleOp>(bf3File, ctx);
            if (!mod) {
                llvm::errs() << "  ✗ Cannot parse " << bf3File << "\n";
                return failure();
            }
            if (failed(emitARMFunction(blockId, *mod, out)))
                return failure();
        }

        // Dispatch table
        out << "/* ARM dispatch table: call nc_arm_blocks[blockId] to run a block */\n"
            << "typedef int (*nc_arm_block_fn)(void *, void *, void *);\n\n";
        // Forward declarations
        for (int id : blocks)
            out << "int nc_block" << id << "_arm(void *, void *, void *);\n";
        out << "\nstatic nc_arm_block_fn nc_arm_blocks[] = {\n";
        for (int id : blocks)
            out << "    nc_block" << id << "_arm,\n";
        out << "};\n\n"
            << "int nc_arm_num_blocks = " << (int)blocks.size() << ";\n";

        return success();
    }
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

std::unique_ptr<Pass> createEmitHandlerCodePass() {
    return std::make_unique<EmitHandlerCodePass>();
}

void registerEmitHandlerCodePass() {
    PassRegistration<EmitHandlerCodePass>();
}

} // namespace mlir
