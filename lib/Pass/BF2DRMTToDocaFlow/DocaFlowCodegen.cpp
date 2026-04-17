// ============================================================================
// File: lib/Pass/BF2DRMTToDocaFlow/DocaFlowCodegen.cpp
//
// DOCA Flow C codegen from bf2drmt dialect.
//
// Reads all bf2drmt.mlir files produced by VDRMTToBF2DRMTPass from inputDir
// and emits inputDir/doca_flow_pipeline.c — a self-contained C source
// implementing the full match-action pipeline as DOCA Flow pipes.
//
// Mapping:
//   bf2drmt.pipe type(basic)   → doca_flow_pipe_cfg + DOCA_FLOW_PIPE_BASIC
//   bf2drmt.table_key          → match struct (field masks)
//   bf2drmt.pipe_action @_hit  → fwd.next_pipe = g_pipes[N]
//   bf2drmt.pipe_action @_miss → fwd_miss.next_pipe = g_pipes[N]
//   bf2drmt.next -1            → fwd.type = DOCA_FLOW_FWD_PORT (egress)
//   bf2drmt.copy_field         → DOCA_FLOW_ACTION_COPY action_desc
//   bf2drmt.add_to_field       → DOCA_FLOW_ACTION_ADD action_desc
//   bf2drmt.assign (FieldCopy) → DOCA_FLOW_ACTION_COPY action_desc
//   bf2drmt.assign (Constant)  → actions.meta.u32[N] set per entry
//
// All DOCA API strings are loaded from a template file (doca-<ver>.toml) via
// DocaFlowTemplate, so this file contains no hardcoded DOCA API literals.
// ============================================================================

#include "Pass/BF2DRMTToDocaFlowPass.h"
#include "Pass/DocaFlowTemplate.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTDialect.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOps.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.h"

using namespace mlir;

namespace {

// ── Data structures ──────────────────────────────────────────────────────────

/// One step in a struct/header field-reference chain.
struct RefPathStep {
    int         fieldIdx;
    std::string fieldName;
    mlir::Type  objectType; // inner object type of the result ref (unwrapped)
};

/// A fully-resolved reference: which block argument + the chain of fields.
/// argIdx: 0=headers, 1=metadata, 2=nc_standard_metadata.
struct RefPath {
    int                      argIdx = -1;
    std::vector<RefPathStep> steps;
    bool valid() const { return argIdx >= 0; }
};

/// Walk a Value backwards through bf2drmt.struct_extract_ref ops.
static RefPath resolveRefChain(mlir::Value val) {
    RefPath path;
    while (true) {
        if (auto arg = mlir::dyn_cast<mlir::BlockArgument>(val)) {
            path.argIdx = (int)arg.getArgNumber();
            std::reverse(path.steps.begin(), path.steps.end());
            return path;
        }
        auto *defOp = val.getDefiningOp();
        if (!defOp) return path;
        auto serOp = mlir::dyn_cast<bf2drmt::StructExtractRefOp>(defOp);
        if (!serOp) return path;

        std::string name = "field" + std::to_string(serOp.getFieldIndex());
        mlir::Type leafObjType;
        if (auto refTy = mlir::dyn_cast<bf2drmt::ReferenceType>(
                serOp.getInput().getType())) {
            auto objTy = refTy.getObjectType();
            auto nameAt = [&](auto containerTy) -> std::string {
                auto elems = containerTy.getElements();
                int idx = serOp.getFieldIndex();
                if (idx >= 0 && (size_t)idx < elems.size())
                    return elems[idx].name.getValue().str();
                return "field" + std::to_string(idx);
            };
            if (auto sTy = mlir::dyn_cast<bf2drmt::StructType>(objTy))
                name = nameAt(sTy);
            else if (auto hTy = mlir::dyn_cast<bf2drmt::HeaderType>(objTy))
                name = nameAt(hTy);
        }
        if (auto resRef = mlir::dyn_cast<bf2drmt::ReferenceType>(
                serOp.getResult().getType()))
            leafObjType = resRef.getObjectType();
        path.steps.push_back({serOp.getFieldIndex(), name, leafObjType});
        val = serOp.getInput();
    }
}

// ── DOCA field lookup (template-backed) ──────────────────────────────────────

struct DocaFieldInfo {
    std::string matchMember; ///< e.g. "outer.ip4.next_proto"
    std::string fieldString; ///< e.g. "outer.ipv4.next_proto"
    std::string extraSetup;  ///< extra match lines (l3/l4 type guards)
    unsigned    widthBits;
};

/// Lookup DOCA field info from the loaded template.
/// Falls back to a visible placeholder comment if the field is unknown.
static DocaFieldInfo lookupDocaField(const DocaFlowTemplate &tmpl,
                                     const std::string &hdrType,
                                     const std::string &fieldName) {
    const DocaFlowTemplate::MatchFieldInfo *mfi =
        tmpl.lookupMatchField(hdrType, fieldName);
    if (mfi)
        return {mfi->matchMember, mfi->fieldString, mfi->extraSetup, mfi->widthBits};
    std::string unknown = "/* unknown:" + hdrType + "." + fieldName + " */";
    return {unknown, unknown, "", 32};
}

static std::string maskForWidth(unsigned bits) {
    if (bits <= 8)  return "0xff";
    if (bits <= 16) return "0xffff";
    return "UINT32_MAX";
}

// ── Action value resolution ───────────────────────────────────────────────────

struct ActionValue {
    enum class Kind { Constant, FieldCopy, FieldCopyMasked, Unknown } kind =
        Kind::Unknown;
    int64_t  constVal    = 0;
    RefPath  srcRef;
    int      copyLowBit  = 0;
    int      copyHighBit = 31;
    unsigned copyWidth   = 32;
};

static ActionValue resolveActionValue(mlir::Value val) {
    ActionValue av;
    auto *defOp = val.getDefiningOp();
    if (!defOp) return av;

    if (auto constOp = mlir::dyn_cast<bf2drmt::ConstantOp>(defOp)) {
        if (auto ia = mlir::dyn_cast<mlir::IntegerAttr>(constOp.getValue())) {
            av.kind = ActionValue::Kind::Constant;
            av.constVal = ia.getInt();
        }
        return av;
    }

    if (auto readOp = mlir::dyn_cast<bf2drmt::ReadOp>(defOp)) {
        av.kind = ActionValue::Kind::FieldCopy;
        av.srcRef = resolveRefChain(readOp.getRef());
        if (auto iT = mlir::dyn_cast<mlir::IntegerType>(readOp.getResult().getType()))
            av.copyWidth = iT.getWidth();
        av.copyHighBit = (int)av.copyWidth - 1;
        return av;
    }

    if (auto sliceOp = mlir::dyn_cast<bf2drmt::SliceOp>(defOp)) {
        if (auto *rDef = sliceOp.getInput().getDefiningOp())
            if (auto readOp = mlir::dyn_cast<bf2drmt::ReadOp>(rDef)) {
                av.kind = ActionValue::Kind::FieldCopyMasked;
                av.srcRef = resolveRefChain(readOp.getRef());
                av.copyLowBit  = (int)sliceOp.getLowBit();
                av.copyHighBit = (int)sliceOp.getHighBit();
                av.copyWidth   = av.copyHighBit - av.copyLowBit + 1;
            }
        return av;
    }

    if (auto binOp = mlir::dyn_cast<bf2drmt::BinOp>(defOp)) {
        if (binOp.getKind() == bf2drmt::BF2DRMTBinOpKind::And) {
            mlir::Value readVal, maskVal;
            if (mlir::dyn_cast_or_null<bf2drmt::ConstantOp>(
                    binOp.getRhs().getDefiningOp()))
                { readVal = binOp.getLhs(); maskVal = binOp.getRhs(); }
            else if (mlir::dyn_cast_or_null<bf2drmt::ConstantOp>(
                    binOp.getLhs().getDefiningOp()))
                { readVal = binOp.getRhs(); maskVal = binOp.getLhs(); }

            if (readVal && maskVal) {
                auto maskConst =
                    mlir::cast<bf2drmt::ConstantOp>(maskVal.getDefiningOp());
                int64_t mask =
                    mlir::cast<mlir::IntegerAttr>(maskConst.getValue()).getInt();
                int lo = 0;
                while (lo < 63 && !(mask >> lo & 1)) lo++;
                int hi = lo;
                while (hi < 62 && (mask >> (hi + 1) & 1)) hi++;
                if (auto *rDef = readVal.getDefiningOp())
                    if (auto readOp = mlir::dyn_cast<bf2drmt::ReadOp>(rDef)) {
                        av.kind = ActionValue::Kind::FieldCopyMasked;
                        av.srcRef = resolveRefChain(readOp.getRef());
                        av.copyLowBit  = lo;
                        av.copyHighBit = hi;
                        av.copyWidth   = hi - lo + 1;
                    }
            }
        }
        return av;
    }
    return av;
}

// ── Field string helpers ──────────────────────────────────────────────────────

/// For metadata destinations: always "meta.data"; slot encoded via bit_offset.
/// For header fields: looked up via the template.
static std::string refPathToDocaFieldStr(const DocaFlowTemplate &tmpl,
                                         const RefPath &rp) {
    if (!rp.valid() || rp.steps.empty()) return "/* unresolved */";
    if (rp.argIdx == 1)
        return "meta.data";
    if (rp.argIdx == 0 && rp.steps.size() >= 2) {
        std::string hdrType;
        if (auto hTy = mlir::dyn_cast<bf2drmt::HeaderType>(rp.steps[0].objectType))
            hdrType = hTy.getName().str();
        return lookupDocaField(tmpl, hdrType, rp.steps[1].fieldName).fieldString;
    }
    return "/* unresolved */";
}

// ── Code generation ───────────────────────────────────────────────────────────

mlir::LogicalResult doGenerate(MLIRContext *ctx, llvm::StringRef inputDir,
                               const DocaFlowTemplate &tmpl,
                               const std::map<int, int> &blockHwMap,
                               const std::map<int, int> &armQueueMap,
                               llvm::raw_ostream &out) {
    // DOCA target version — gates structural differences that can't live in
    // the template (no destroy-label, value-struct pipe_cfg, monitor at
    // function scope, nc_required_resources helper for resource prebudget).
    const bool is22 = (tmpl.get("meta.doca_version") == "2.2");

    struct PipeRecord {
        int blockId;
        mlir::OwningOpRef<mlir::ModuleOp> module;
        bf2drmt::PipeOp pipeOp;
    };
    std::vector<PipeRecord> pipes;

    std::error_code ec;
    for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
         dir != end && !ec; dir.increment(ec)) {
        auto path = dir->path();
        if (!llvm::sys::fs::is_directory(path)) continue;
        std::string dirName = llvm::sys::path::filename(path).str();
        if (dirName.find("block") != 0) continue;

        std::string bf3File = path + "/bf2drmt.mlir";
        if (!llvm::sys::fs::exists(bf3File)) continue;

        auto mod = parseSourceFile<ModuleOp>(bf3File, ctx);
        if (!mod) continue;

        for (auto pipeOp : mod->getOps<bf2drmt::PipeOp>()) {
            std::string sym = pipeOp.getSymName().str();
            std::string digits;
            for (char c : sym) if (std::isdigit(c)) digits += c;
            int id = digits.empty() ? -1 : std::stoi(digits);
            pipes.push_back({id, std::move(mod), pipeOp});
            break;
        }
    }
    if (pipes.empty()) {
        llvm::errs() << "  ✗ No bf2drmt.mlir files to generate from\n";
        return mlir::failure();
    }
    llvm::sort(pipes, [](const PipeRecord &a, const PipeRecord &b) {
        return a.blockId < b.blockId;
    });
    int nPipes = (int)pipes.size();

    // ── Collect counter / meter declarations from all modules ──────────────
    struct CounterDeclInfo  { std::string name; int32_t size; };
    struct MeterDeclInfo    { std::string name; int32_t size; int32_t meterType; };
    struct RegisterDeclInfo { std::string name; int32_t size; int32_t elementWidth; };
    std::vector<CounterDeclInfo>  allCounterDecls;
    std::vector<MeterDeclInfo>    allMeterDecls;
    std::vector<RegisterDeclInfo> allRegisterDecls;
    {
        std::set<std::string> seenC, seenM, seenR;
        for (auto &pr : pipes) {
            pr.module->walk([&](bf2drmt::CounterDeclOp op) {
                std::string n = op.getSymName().str();
                if (seenC.insert(n).second)
                    allCounterDecls.push_back({n, (int32_t)op.getSize()});
            });
            pr.module->walk([&](bf2drmt::MeterDeclOp op) {
                std::string n = op.getSymName().str();
                if (seenM.insert(n).second)
                    allMeterDecls.push_back({n, (int32_t)op.getSize(),
                                            (int32_t)op.getMeterType()});
            });
            pr.module->walk([&](bf2drmt::RegisterDeclOp op) {
                std::string n = op.getSymName().str();
                if (seenR.insert(n).second)
                    allRegisterDecls.push_back({n, (int32_t)op.getSize(),
                                               (int32_t)op.getElementWidth()});
            });
        }
    }
    bool hasAnyMeters = !allMeterDecls.empty();

    // g_pipes must be indexed by block ID (not pipe count).
    // Find the max block ID referenced by ANY pipe (including successor targets
    // that may be terminal blocks not in the DRMT pipe list).
    int maxDrmtBlockId = -1;
    for (auto &pr : pipes) {
        if (pr.blockId > maxDrmtBlockId) maxDrmtBlockId = pr.blockId;
        // Explicitly iterate pipe action bodies to find successor block IDs.
        auto &pipeBody = pr.pipeOp.getBody().front();
        for (auto &op : pipeBody) {
            if (auto paOp = mlir::dyn_cast<bf2drmt::PipeActionOp>(&op)) {
                for (auto &aop : paOp.getBody().front()) {
                    if (auto nextOp = mlir::dyn_cast<bf2drmt::NextOp>(&aop)) {
                        int s = (int)nextOp.getSuccessor();
                        if (s > maxDrmtBlockId) maxDrmtBlockId = s;
                    }
                }
            }
        }
    }
    int gPipesSize = maxDrmtBlockId + 1;

    // Successor map: blockId → [successor blockIds] (DRMT-only successors for topo sort)
    std::map<int, std::vector<int>> successors;
    for (auto &pr : pipes) {
        pr.pipeOp->walk([&](bf2drmt::NextOp nextOp) {
            int s = (int)nextOp.getSuccessor();
            // Only track DRMT successors for topological ordering
            if (s >= 0) {
                auto it = blockHwMap.find(s);
                bool isDrmt = (it == blockHwMap.end() || it->second == 0);
                if (isDrmt) successors[pr.blockId].push_back(s);
            }
        });
    }

    // ── File header ────────────────────────────────────────────────────────
    out << "/*\n"
        << " * Auto-generated DOCA Flow pipeline\n"
        << " * Generated by nutcracker BF2DRMTToDocaFlow codegen\n"
        << " *\n"
        << " * Build with:\n"
        << " *   gcc -o pipeline doca_flow_pipeline.c \\\n"
        << " *       -I/opt/mellanox/doca/include \\\n"
        << " *       -L/opt/mellanox/doca/lib/aarch64-linux-gnu \\\n"
        << " *       -ldoca_flow -ldoca_common\n"
        << " */\n\n"
        << "#include <stddef.h>\n"
        << "#include <stdint.h>\n"
        << "#include <string.h>\n"
        << "#include <stdbool.h>\n"
        << "#include <doca_log.h>\n"
        << "#include <doca_flow.h>\n\n"
        << "/* Inline replacement for flow_common.h entries_status */\n"
        << "struct entries_status {\n"
        << "    bool failure;\n"
        << "    int  nb_processed;\n"
        << "};\n\n"
        << "/* Byte offset of meta.u32[idx] converted to bits (for field_op.bit_offset) */\n"
        << "#define META_U32_BIT_OFFSET(idx) \\\n"
        << "    ((uint32_t)(offsetof(struct doca_flow_meta, u32[(idx)]) << 3))\n\n"
        << "/* DPA RSS queue base: queue (NC_DPA_QUEUE_BASE + block_id) */\n"
        << "#define NC_DPA_QUEUE_BASE 16\n\n"
        << "DOCA_LOG_REGISTER(NC_PIPELINE);\n\n"
        << "#define NC_NB_DRMT_PIPES " << nPipes << "\n"
        << "#define NB_ACTIONS_ARR 1\n\n"
        << "/* g_pipes indexed by block ID (size = max DRMT block ID + 1) */\n"
        << "static struct doca_flow_pipe *g_pipes[" << gPipesSize << "];\n\n";

    // ── Counter declarations ───────────────────────────────────────────────
    if (!allCounterDecls.empty()) {
        out << "/* === Per-entry hardware counters (non-shared, one per pipe entry) === */\n";
        for (auto &cd : allCounterDecls) {
            // Convert name to upper-case macro prefix
            std::string upper = cd.name;
            for (char &c : upper) c = (char)std::toupper((unsigned char)c);
            out << "#define NC_COUNTER_" << upper << "_SIZE " << cd.size << "\n";
        }
        out << "\n";
    }

    // ── Register declarations ──────────────────────────────────────────────
    if (!allRegisterDecls.empty()) {
        out << "/* === Stateful register arrays ===\n"
            << " * NOTE: On BF3 hardware these require DPA-side backing.\n"
            << " * The arrays below are CPU-accessible stubs for simulation.\n"
            << " * Replace NC_REG_READ / NC_REG_WRITE with DPA calls in production.\n"
            << " */\n";
        for (auto &rd : allRegisterDecls) {
            std::string upper = rd.name;
            for (char &c : upper) c = (char)std::toupper((unsigned char)c);
            int byteWidth = (rd.elementWidth + 7) / 8;
            // Choose the smallest standard C type that fits.
            std::string ctype = (byteWidth <= 1) ? "uint8_t"
                              : (byteWidth <= 2) ? "uint16_t"
                              : (byteWidth <= 4) ? "uint32_t" : "uint64_t";
            out << "#define NC_REG_" << upper << "_SIZE " << rd.size << "\n";
            out << "static " << ctype << " nc_reg_" << rd.name
                << "[NC_REG_" << upper << "_SIZE];\n";
            out << "#define NC_REG_READ_"  << upper << "(idx) nc_reg_" << rd.name << "[(idx)]\n";
            out << "#define NC_REG_WRITE_" << upper << "(idx, val) (nc_reg_" << rd.name << "[(idx)] = (val))\n";
        }
        out << "\n";
    }

    // ── Meter declarations + init function ────────────────────────────────
    if (hasAnyMeters) {
        out << "/* === Shared meter resources (DOCA_FLOW_SHARED_RESOURCE_METER) === */\n";
        for (auto &md : allMeterDecls) {
            std::string upper = md.name;
            for (char &c : upper) c = (char)std::toupper((unsigned char)c);
            out << "#define NC_METER_" << upper << "_SIZE " << md.size
                << " /* meter_type=" << md.meterType
                << (md.meterType == 0 ? " (packets)" : " (bytes)") << " */\n";
        }
        out << "\n";

        out << "/* Initialize all shared meter resources on the given port.\n"
            << " * Call once from nc_setup_pipeline before creating pipes.\n"
            << " * Default rates: CIR=1Mpps, CBS=10000, EBS=10000 -- tune per use-case. */\n"
            << "static doca_error_t nc_init_shared_resources(struct doca_flow_port *port)\n"
            << "{\n"
            << "    " << tmpl.render("shared_resource.meter.cfg_locals") << "\n"
            << "    doca_error_t result;\n\n";
        for (auto &md : allMeterDecls) {
            std::string upper = md.name;
            for (char &c : upper) c = (char)std::toupper((unsigned char)c);
            std::string szMacro = "NC_METER_" + upper + "_SIZE";
            std::string limitType = tmpl.lookupEnum("meter_limit",
                md.meterType == 0 ? "packets" : "bytes");
            out << "    /* Meter: " << md.name << " (" << szMacro << " slots, "
                << (md.meterType == 0 ? "packets" : "bytes") << ") */\n"
                << "    {\n"
                << "        uint32_t meter_ids[" << szMacro << "];\n"
                << "        memset(&cfg, 0, sizeof(cfg));\n"
                << "        for (uint32_t i = 0; i < " << szMacro << "; i++) {\n"
                << "            " << tmpl.render("shared_resource.meter.cfg_set", {
                    {"i",           "i"},
                    {"limit_type",  limitType},
                    {"cir",         "1000000ULL"},
                    {"cbs",         "10000"},
                }) << "\n"
                << "            meter_ids[i] = i;\n"
                << "        }\n"
                << "        " << tmpl.render("shared_resource.meter.bind", {
                    {"n_meters", szMacro},
                }) << "\n"
                << "    }\n\n";
        }
        out << "    return DOCA_SUCCESS;\n}\n\n";
    }

    // 2.2-only resource-pool totals, used by nc_required_resources() helper
    // emitted after the per-pipe loop. Per-entry non-shared counters draw
    // from the doca_flow_cfg.resource pool pre-sized at doca_flow_init time.
    int totalNbCounters = 0;
    int totalNbMeters   = 0;
    for (auto &md : allMeterDecls) totalNbMeters += md.size;

    // Set of block IDs that exist as BF2 DRMT pipes.  Successor references
    // to blocks outside this set (DPA/ARM blocks skipped by BF2) must fall
    // back to FWD_PORT (egress) instead of FWD_PIPE (would dereference NULL).
    std::set<int> pipeBlockIds;
    for (auto &pr : pipes) pipeBlockIds.insert(pr.blockId);

    // ── Per-pipe functions ─────────────────────────────────────────────────
    for (auto &pr : pipes) {
        int id = pr.blockId;
        bf2drmt::PipeOp pipeOp = pr.pipeOp;
        std::string pipeName = "NC_BLOCK" + std::to_string(id);

        std::string pipeTypeKey = "basic";
        switch (pipeOp.getPipeType()) {
            case bf2drmt::BF2DRMTPipeType::Acl:     pipeTypeKey = "acl";     break;
            case bf2drmt::BF2DRMTPipeType::Control: pipeTypeKey = "control"; break;
            case bf2drmt::BF2DRMTPipeType::Hash:    pipeTypeKey = "hash";    break;
            case bf2drmt::BF2DRMTPipeType::Lpm:     pipeTypeKey = "lpm";     break;
            default: break;
        }
        std::string pipeTypeStr = tmpl.lookupEnum("pipe_type", pipeTypeKey);

        struct MatchField  { DocaFieldInfo doca; unsigned widthBits; };
        struct AssignSlot  { RefPath dst; ActionValue value; bool isAddToField = false; };
        struct RegAccess {
            std::string instance;
            bool        isWrite;
            ActionValue index;  // array index
            ActionValue value;  // for writes: value to write
        };
        struct ActionRegion {
            std::string actionSym;
            int nextBlock;
            std::vector<AssignSlot> assigns;
            std::string counterInstance; // non-empty if action has bf2drmt.counter.count
            std::string meterInstance;   // non-empty if action has bf2drmt.meter.execute
            std::vector<RegAccess> registerAccesses; // register.read / register.write ops
        };

        std::vector<MatchField>   matchFields;
        std::vector<ActionRegion> actionRegions;
        bool        hasTableKey = false;
        std::string extraMatchSetup;

        bool isHashPipe = (pipeOp.getPipeType() == bf2drmt::BF2DRMTPipeType::Hash);
        int32_t nrEntries = pipeOp.getNrEntries().value_or(64);

        auto &body = pipeOp.getBody().front();
        for (auto &op : body) {
            if (auto tkOp = mlir::dyn_cast<bf2drmt::TableKeyOp>(&op)) {
                hasTableKey = true;
                if (isHashPipe) {
                    // For hash pipes, collect ALL ReadOps in the table_key region
                    // (each Read corresponds to one hash function input field).
                    tkOp.getBody().front().walk([&](bf2drmt::ReadOp rdOp) {
                        RefPath rp = resolveRefChain(rdOp.getRef());
                        if (!rp.valid() || rp.argIdx != 0 || rp.steps.size() < 2)
                            return;
                        std::string hdrType;
                        if (auto hTy = mlir::dyn_cast<bf2drmt::HeaderType>(
                                rp.steps[0].objectType))
                            hdrType = hTy.getName().str();
                        // Skip P4 validity flags — they are not real packet fields.
                        if (rp.steps[1].fieldName == "__valid") return;
                        auto doca = lookupDocaField(tmpl, hdrType, rp.steps[1].fieldName);
                        if (!doca.extraSetup.empty() &&
                            extraMatchSetup.find(doca.extraSetup) == std::string::npos)
                            extraMatchSetup += doca.extraSetup + "\n    ";
                        matchFields.push_back({doca, doca.widthBits});
                    });
                } else {
                    tkOp.getBody().front().walk([&](bf2drmt::MatchKeyOp mkOp) {
                        auto *kDef = mkOp.getKey().getDefiningOp();
                        if (!kDef) return;
                        mlir::Value refToRead;
                        if (auto rd = mlir::dyn_cast<bf2drmt::ReadOp>(kDef))
                            refToRead = rd.getRef();
                        if (!refToRead) return;
                        RefPath rp = resolveRefChain(refToRead);
                        if (!rp.valid() || rp.argIdx != 0 || rp.steps.size() < 2)
                            return;
                        std::string hdrType;
                        if (auto hTy = mlir::dyn_cast<bf2drmt::HeaderType>(
                                rp.steps[0].objectType))
                            hdrType = hTy.getName().str();
                        // Skip P4 validity flags — they are not real packet fields.
                        if (rp.steps[1].fieldName == "__valid") return;
                        auto doca = lookupDocaField(tmpl, hdrType, rp.steps[1].fieldName);
                        if (!doca.extraSetup.empty() &&
                            extraMatchSetup.find(doca.extraSetup) == std::string::npos)
                            extraMatchSetup += doca.extraSetup + "\n    ";
                        matchFields.push_back({doca, doca.widthBits});
                    });
                }
                continue;
            }
            if (auto paOp = mlir::dyn_cast<bf2drmt::PipeActionOp>(&op)) {
                ActionRegion ar;
                ar.actionSym = paOp.getAction().getRootReference().str();
                ar.nextBlock = -1;
                for (auto &aop : paOp.getBody().front()) {
                    if (auto nextOp = mlir::dyn_cast<bf2drmt::NextOp>(&aop))
                        ar.nextBlock = (int)nextOp.getSuccessor();
                    else if (auto assignOp = mlir::dyn_cast<bf2drmt::AssignOp>(&aop))
                        ar.assigns.push_back({resolveRefChain(assignOp.getRef()),
                                              resolveActionValue(assignOp.getValue()), false});
                    else if (auto addOp = mlir::dyn_cast<bf2drmt::AddToFieldOp>(&aop)) {
                        ActionValue av;
                        av.kind         = ActionValue::Kind::FieldCopy;
                        av.srcRef       = resolveRefChain(addOp.getSrcRef());
                        av.copyLowBit   = (int)addOp.getSrcOffset();
                        av.copyHighBit  = (int)addOp.getSrcOffset() + (int)addOp.getWidth() - 1;
                        av.copyWidth    = (unsigned)addOp.getWidth();
                        ar.assigns.push_back({resolveRefChain(addOp.getDstRef()), av, true});
                    }
                    else if (auto cntOp = mlir::dyn_cast<bf2drmt::CounterCountOp>(&aop))
                        ar.counterInstance = cntOp.getInstance().str();
                    else if (auto meterOp = mlir::dyn_cast<bf2drmt::MeterExecuteOp>(&aop))
                        ar.meterInstance = meterOp.getInstance().str();
                    else if (auto regRead = mlir::dyn_cast<bf2drmt::RegisterReadOp>(&aop)) {
                        RegAccess ra;
                        ra.instance = regRead.getInstance().str();
                        ra.isWrite  = false;
                        ra.index    = resolveActionValue(regRead.getIndex());
                        ar.registerAccesses.push_back(std::move(ra));
                    } else if (auto regWrite = mlir::dyn_cast<bf2drmt::RegisterWriteOp>(&aop)) {
                        RegAccess ra;
                        ra.instance = regWrite.getInstance().str();
                        ra.isWrite  = true;
                        ra.index    = resolveActionValue(regWrite.getIndex());
                        ra.value    = resolveActionValue(regWrite.getValue());
                        ar.registerAccesses.push_back(std::move(ra));
                    }
                }
                actionRegions.push_back(std::move(ar));
                continue;
            }
        }

        // DOCA HWS cannot build a multi-entry table when the match template is
        // completely empty (nb_flows > 0 + empty match → "items build failed").
        // Per the DOCA flow_hairpin_vnf sample, a catch-all BASIC pipe leaves
        // nb_flows at 0 (the default) and uses a single wildcard entry.
        if (matchFields.empty() && !isHashPipe)
            nrEntries = 0;

        // Count action descriptors needed
        int nDescs = 0;
        for (auto &ar : actionRegions)
            for (auto &sl : ar.assigns)
                if (sl.value.kind == ActionValue::Kind::FieldCopy ||
                    sl.value.kind == ActionValue::Kind::FieldCopyMasked)
                    nDescs++;
        bool hasActionDescs = nDescs > 0;

        bool hasMiss = false;
        for (auto &ar : actionRegions)
            if (ar.actionSym == "_miss") hasMiss = true;

        // Detect counter / meter / register usage in action bodies
        bool hasCounter  = false;
        bool hasMeter    = false;
        bool hasRegister = false;
        std::string pipeCounterInstance, pipeMeterInstance;
        for (auto &ar : actionRegions) {
            if (!ar.counterInstance.empty())    { hasCounter  = true; pipeCounterInstance = ar.counterInstance; }
            if (!ar.meterInstance.empty())      { hasMeter    = true; pipeMeterInstance   = ar.meterInstance; }
            if (!ar.registerAccesses.empty())   { hasRegister = true; }
        }
        if (hasCounter) totalNbCounters += nrEntries;

        // Hash pipes that only forward (no assigns, no counter/meter) don't
        // need an actions template — the DOCA flow_hash_pipe sample omits it.
        // Passing a zero-filled actions struct causes "failed to modify pipe
        // DPDK actions" at entry-add time.
        bool hasAnyActions = hasActionDescs || hasCounter || hasMeter;
        for (auto &ar : actionRegions)
            for (auto &sl : ar.assigns) { hasAnyActions = true; break; }
        bool skipActions = isHashPipe && !hasAnyActions;

        // ── nc_create_pipe_N(port, port_id) ──────────────────────────────
        out << "/* ── Block " << id << ": " << pipeName
            << " ──────────────────────────── */\n"
            << "static doca_error_t nc_create_pipe_" << id
            << "(struct doca_flow_port *port, int port_id)\n{\n"
            << "    struct doca_flow_match match;\n";
        if (!skipActions)
            out << "    struct doca_flow_actions actions,"
                   " *actions_arr[NB_ACTIONS_ARR] = {&actions};\n";
        if (hasActionDescs) {
            out << "    struct doca_flow_action_desc desc_array[" << nDescs << "];\n"
                << "    struct doca_flow_action_descs descs,"
                   " *descs_arr[NB_ACTIONS_ARR] = {&descs};\n";
        }
        out << "    struct doca_flow_fwd fwd;\n";
        if (hasMiss || hasTableKey)
            out << "    struct doca_flow_fwd fwd_miss;\n";
        out << "    " << tmpl.render("pipe.cfg_locals") << "\n"
            << "    doca_error_t result;\n\n"
            << "    memset(&match,   0, sizeof(match));\n";
        if (!skipActions)
            out << "    memset(&actions, 0, sizeof(actions));\n";
        out << "    memset(&fwd,     0, sizeof(fwd));\n";
        if (hasMiss || hasTableKey)
            out << "    memset(&fwd_miss, 0, sizeof(fwd_miss));\n";
        if (hasActionDescs)
            out << "    memset(desc_array, 0, sizeof(desc_array));\n"
                << "    memset(&descs,     0, sizeof(descs));\n";
        out << "\n";

        // Match / match_mask fields
        // Hash pipe: fields go into match_mask (hash function selection), not match.
        if (!matchFields.empty()) {
            std::string matchVar = isHashPipe ? "match_mask" : "match";
            out << "    /* " << (isHashPipe ? "Hash function fields (match_mask)" : "Match key fields") << " */\n";
            if (isHashPipe)
                out << "    struct doca_flow_match match_mask;\n"
                    << "    memset(&match_mask, 0, sizeof(match_mask));\n";
            if (!extraMatchSetup.empty())
                out << "    " << extraMatchSetup << "\n";
            for (auto &mf : matchFields)
                if (!mf.doca.matchMember.empty())
                    out << "    " << matchVar << "." << mf.doca.matchMember
                        << " = " << maskForWidth(mf.widthBits) << ";\n";
            out << "\n";
        }

        // Action descriptors: copy_field / add_to_field / assign(FieldCopy)
        if (hasActionDescs) {
            out << "    /* Action descriptors */\n";
            int di = 0;
            for (auto &ar : actionRegions) {
                for (auto &sl : ar.assigns) {
                    if (sl.value.kind != ActionValue::Kind::FieldCopy &&
                        sl.value.kind != ActionValue::Kind::FieldCopyMasked)
                        continue;
                    std::string srcStr = refPathToDocaFieldStr(tmpl, sl.value.srcRef);
                    std::string dstStr = refPathToDocaFieldStr(tmpl, sl.dst);
                    std::string dstBitOff = "0";
                    if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty())
                        dstBitOff = "META_U32_BIT_OFFSET("
                                    + std::to_string(sl.dst.steps[0].fieldIdx) + ")";
                    // Determine descriptor kind from op type (copy vs add)
                    bool isAdd = sl.isAddToField;
                    std::string descKey = isAdd ? "action_desc.add" : "action_desc.copy";
                    out << tmpl.render(descKey, {
                        {"i",          std::to_string(di)},
                        {"src_field",  srcStr},
                        {"dst_field",  dstStr},
                        {"src_offset", std::to_string(sl.value.copyLowBit)},
                        {"dst_offset", dstBitOff},
                    }) << "\n";
                    if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty())
                        out << "    actions.meta.u32[" << sl.dst.steps[0].fieldIdx
                            << "] = UINT32_MAX;\n";
                    di++;
                }
            }
            out << "    descs.nb_action_desc = " << di << ";\n"
                << "    descs.desc_array = desc_array;\n\n";
        }

        // Constant assigns: set mask in pipe template
        for (auto &ar : actionRegions)
            for (auto &sl : ar.assigns) {
                if (sl.value.kind != ActionValue::Kind::Constant) continue;
                if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty())
                    out << "    " << tmpl.render("action_desc.set_meta", {
                        {"idx", std::to_string(sl.dst.steps[0].fieldIdx)},
                        {"val", "UINT32_MAX"},
                    }) << " /* mask: constant set per entry */\n";
            }

        // Pipe creation
        out << "\n" << tmpl.render("pipe.cfg_create", {
            {"name",      pipeName},
            {"pipe_type", pipeTypeStr},
            {"nr_entries",std::to_string(nrEntries)},
        }) << "\n";

        if (hasTableKey)
            out << tmpl.render("pipe.cfg_set_root") << "\n";

        if (isHashPipe)
            out << tmpl.render(matchFields.empty()
                    ? "pipe.cfg_set_match_with_mask"   // NULL match_mask
                    : "pipe.cfg_set_match_with_mask") << "\n";
        else
            out << tmpl.render("pipe.cfg_set_match_no_mask") << "\n";

        if (skipActions) {
            // Forward-only hash pipe — no actions template (matches DOCA
            // flow_hash_pipe sample which omits pipe_cfg.actions entirely).
        } else if (is22) {
            out << tmpl.render("pipe.cfg_set_actions") << "\n";
            if (hasActionDescs)
                out << tmpl.render("pipe.cfg_set_action_descs", {
                    {"descs_arr_ptr", "descs_arr"},
                }) << "\n";
        } else {
            out << tmpl.render("pipe.cfg_set_actions", {
                {"descs_arr_ptr", hasActionDescs ? "&descs" : "NULL"},
            }) << "\n";
        }

        // Monitor: counter and/or meter
        if (hasCounter || hasMeter) {
            out << "    /* Monitor: ";
            if (hasCounter) out << "per-entry hardware counter (@" << pipeCounterInstance << ")";
            if (hasCounter && hasMeter) out << " + ";
            if (hasMeter)   out << "shared meter (@" << pipeMeterInstance << ")";
            out << " */\n";
            if (is22) {
                // 2.2: monitor must stay alive until doca_flow_pipe_create reads
                // pipe_cfg.monitor (pointer capture, not field copy). Declare at
                // function scope.
                out << "    " << tmpl.render("monitor.locals") << "\n";
                if (hasCounter)
                    out << "    " << tmpl.render("monitor.set_counter") << "\n";
                if (hasMeter)
                    out << "    " << tmpl.render("monitor.set_meter") << "\n";
                out << tmpl.render("pipe.cfg_set_monitor") << "\n\n";
                // NB: `actions.shared.*` (2.9-only shared-meter action binding)
                // intentionally omitted — shared-meter path is deferred on BF2.
            } else {
                out << "    {\n";
                out << "        " << tmpl.render("monitor.locals") << "\n";
                if (hasCounter)
                    out << "        " << tmpl.render("monitor.set_counter") << "\n";
                if (hasMeter)
                    out << "        " << tmpl.render("monitor.set_meter") << "\n";
                out << tmpl.render("pipe.cfg_set_monitor") << "\n    }\n\n";

                if (hasMeter) {
                    out << "    /* Meter action template: bind shared meter (res_id overridden per entry) */\n"
                        << "    actions.shared.type = DOCA_FLOW_SHARED_RESOURCE_METER;\n"
                        << "    actions.shared.res_id = UINT32_MAX; /* wildcard mask */\n\n";
                }
            }
        }

        // Fwd / fwd_miss
        int hitNext = -1, missNext = -1, defaultNext = -1;
        for (auto &ar : actionRegions) {
            if (ar.actionSym == "_hit")          hitNext     = ar.nextBlock;
            else if (ar.actionSym == "_miss")    missNext    = ar.nextBlock;
            else if (ar.actionSym == "_default") defaultNext = ar.nextBlock;
        }
        int mainNext = (hitNext >= 0) ? hitNext : defaultNext;

        auto emitFwd = [&](const std::string &var, int nextId) {
            if (nextId == -1 || !pipeBlockIds.count(nextId)) {
                // No successor, or successor is a DPA/ARM block not in the
                // BF2 pipe set — egress via port (matches FWD_PORT pattern).
                out << "    " << tmpl.render("fwd.port", {{"var", var}}) << "\n";
                return;
            }
            auto hwIt = blockHwMap.find(nextId);
            int hw = (hwIt != blockHwMap.end()) ? hwIt->second : 0;
            if (hw == 2) { // ARM
                auto qIt = armQueueMap.find(nextId);
                int qidx = (qIt != armQueueMap.end()) ? qIt->second : 0;
                out << "    " << tmpl.render("fwd.rss", {
                    {"var", var}, {"l4", "UDP | DOCA_FLOW_RSS_TCP"},
                }) << "\n"
                    << "    " << var << ".rss_queues = (uint16_t[]){ " << qidx
                    << " }; /* ARM block " << nextId << " queue */\n"
                    << "    " << var << ".num_of_queues = 1;\n";
            } else if (hw == 1) { // DPA
                out << "    " << tmpl.render("fwd.rss", {
                    {"var", var}, {"l4", "UDP | DOCA_FLOW_RSS_TCP"},
                }) << "\n"
                    << "    " << var << ".rss_queues = (uint16_t[]){ (uint16_t)"
                    << "(NC_DPA_QUEUE_BASE + " << nextId << ") };"
                    << " /* DPA block " << nextId << " */\n"
                    << "    " << var << ".num_of_queues = 1;\n";
            } else { // DRMT pipe chain
                out << "    " << tmpl.render("fwd.pipe", {
                    {"var",     var},
                    {"next_id", std::to_string(nextId)},
                }) << " /* block " << nextId << " */\n";
            }
        };

        emitFwd("fwd", mainNext);
        // DOCA 2.2 HWS rejects non-NULL fwd_miss on BASIC pipes — the pipe
        // builder's "items build" step fails with rc=-22. All DOCA 2.2 samples
        // pass NULL for fwd_miss on BASIC/hash pipes. On the 2.9 builder path
        // fwd_miss is supported via a separate builder call, so only suppress
        // on 2.2.
        bool hasMissFwd = (hasMiss || (hasTableKey && missNext >= 0))
                          && !is22;
        if (hasMissFwd) { out << "\n"; emitFwd("fwd_miss", missNext); }

        out << "\n" << tmpl.render("pipe.create", {
            {"id",           std::to_string(id)},
            {"miss_fwd_ptr", hasMissFwd ? "&fwd_miss" : "NULL"},
        }) << "\n";
        if (is22) {
            // 2.2: pipe_cfg is a stack value-struct, no destroy API. The
            // template's pipe.create already returns on error; happy path
            // falls through here.
            out << "    return DOCA_SUCCESS;\n}\n\n";
        } else {
            out << "destroy:\n"
                << "    doca_flow_pipe_cfg_destroy(pipe_cfg);\n"
                << "    return result;\n"
                << "}\n\n";
        }

        // ── nc_add_pipe_N_entry() ──────────────────────────────────────────
        out << "static doca_error_t nc_add_pipe_" << id
            << "_entry(struct entries_status *status)\n{\n";
        if (!skipActions)
            out << "    struct doca_flow_actions actions;\n";
        out << "    struct doca_flow_pipe_entry *entry;\n"
            << "    doca_error_t result;\n\n";
        if (!skipActions)
            out << "    memset(&actions, 0, sizeof(actions));\n\n";

        if (isHashPipe) {
            if (!skipActions)
                out << "    actions.action_idx = 0;\n\n";
            out << "    /* Add one entry per hash bucket */\n"
                << "    for (uint32_t i = 0; i < " << nrEntries << "; i++) {\n";
            if (hasMeter && !is22) {
                out << "        /* Bind shared meter slot i to bucket i */\n"
                    << "        actions.shared.type = DOCA_FLOW_SHARED_RESOURCE_METER;\n"
                    << "        actions.shared.res_id = i;\n";
            }
            out << "        " << tmpl.render("entry.add_hash", {
                {"id",          std::to_string(id)},
                {"hash_idx",    "i"},
                {"actions_ptr", skipActions ? "NULL" : "&actions"},
                {"monitor_ptr", "NULL"},
                {"flags",       tmpl.get("entry.flags_nowait")},
            }) << "\n"
                << "    }\n"
                << "    return DOCA_SUCCESS;\n"
                << "}\n\n";
        } else {
            out << "    struct doca_flow_match match;\n"
                << "    memset(&match, 0, sizeof(match));\n\n";

            bool hasEntryActions = false;
            for (auto &ar : actionRegions)
                for (auto &sl : ar.assigns) {
                    if (sl.value.kind != ActionValue::Kind::Constant) continue;
                    if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty()) {
                        out << "    " << tmpl.render("action_desc.set_meta", {
                            {"idx", std::to_string(sl.dst.steps[0].fieldIdx)},
                            {"val", std::to_string(sl.value.constVal)},
                        }) << "\n";
                        hasEntryActions = true;
                    }
                }

            if (!hasEntryActions && matchFields.empty())
                out << "    /* No match key or constant actions for this block */\n";
            else if (!matchFields.empty())
                out << "    /* TODO: set match fields to runtime key values */\n";

            out << "\n    actions.action_idx = 0;\n"
                << "    " << tmpl.render("entry.add", {
                    {"id",          std::to_string(id)},
                    {"monitor_ptr", "NULL"},
                    {"flags",       tmpl.get("entry.flags_nowait")},
                }) << "\n"
                << "    return result;\n"
                << "}\n\n";
        }

        // ── nc_pipe_N_reg_ops(): software-side register accesses ────────────
        // DOCA Flow has no native stateful register primitive; register read/write
        // ops must run in a software packet handler (e.g. DPA thread or ARM callback).
        // This stub shows what operations the block performs so you can call it from
        // the appropriate handler.
        if (hasRegister) {
            // Helper: render an ActionValue as a C expression string.
            auto avToStr = [&](const ActionValue &av, const std::string &placeholder) -> std::string {
                if (av.kind == ActionValue::Kind::Constant)
                    return std::to_string(av.constVal);
                return placeholder;
            };

            out << "/* Register ops for block " << id << " – call from packet processing handler */\n"
                << "static inline void nc_pipe_" << id << "_reg_ops"
                << "(uint32_t idx, uint32_t val)\n{\n"
                << "    (void)idx; (void)val;\n";
            for (auto &ar : actionRegions) {
                for (auto &ra : ar.registerAccesses) {
                    std::string upper = ra.instance;
                    for (char &c : upper) c = (char)std::toupper((unsigned char)c);
                    std::string idxStr = avToStr(ra.index, "idx");
                    if (ra.isWrite) {
                        std::string valStr = avToStr(ra.value, "val");
                        out << "    NC_REG_WRITE_" << upper << "(" << idxStr
                            << ", " << valStr << ");\n";
                    } else {
                        out << "    (void)NC_REG_READ_" << upper << "(" << idxStr << ");\n";
                    }
                }
            }
            out << "}\n\n";
        }
    }

    // ── nc_required_resources() — 2.2 only ─────────────────────────────────
    // 2.2 requires counter/meter pool sizes on doca_flow_cfg.resource BEFORE
    // doca_flow_init(). Caller invokes this during app init to get the totals.
    if (is22) {
        out << "/* DOCA 2.2: caller must populate doca_flow_cfg.resource with\n"
            << " * these values BEFORE doca_flow_init(). */\n"
            << "void nc_required_resources(uint32_t *nb_counters, uint32_t *nb_meters)\n"
            << "{\n"
            << "    *nb_counters = " << totalNbCounters << ";\n"
            << "    *nb_meters   = " << totalNbMeters   << ";\n"
            << "}\n\n";
    }

    // ── nc_setup_pipeline() ────────────────────────────────────────────────
    // Pipes must be created in reverse topological order so that fwd.next_pipe
    // pointers already point to valid pipe objects at creation time.
    std::set<int> allIds;
    for (auto &pr : pipes) allIds.insert(pr.blockId);

    std::vector<int> createOrder;
    std::set<int> placed;
    while ((int)placed.size() < nPipes) {
        bool progress = false;
        for (auto &pr : pipes) {
            int id = pr.blockId;
            if (placed.count(id)) continue;
            bool ready = true;
            for (int s : successors[id])
                if (allIds.count(s) && !placed.count(s)) { ready = false; break; }
            if (ready) {
                createOrder.push_back(id);
                placed.insert(id);
                progress = true;
            }
        }
        if (!progress) {
            for (auto &pr : pipes)
                if (!placed.count(pr.blockId)) {
                    createOrder.push_back(pr.blockId);
                    placed.insert(pr.blockId);
                }
            break;
        }
    }

    out << "/* ── Pipeline setup ──────────────────────────────────────────── */\n"
        << "doca_error_t nc_setup_pipeline(struct doca_flow_port *port, int port_id,\n"
        << "                               struct entries_status *status)\n{\n"
        << "    doca_error_t result;\n\n";
    if (hasAnyMeters) {
        out << "    /* Initialize shared meter resources before creating pipes */\n"
            << "    result = nc_init_shared_resources(port);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Failed to init shared resources: %s\", "
            << tmpl.get("error.to_string") << "(result));\n"
            << "        return result;\n"
            << "    }\n\n";
    }
    out << "    /* Create pipes in reverse topological order (leaves first) */\n";
    for (int id : createOrder) {
        out << "    result = nc_create_pipe_" << id << "(port, port_id);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Failed to create pipe " << id
            << ": %s\", " << tmpl.get("error.to_string") << "(result));\n"
            << "        return result;\n"
            << "    }\n";
    }
    out << "\n    /* Add default entries */\n";
    for (int id : createOrder) {
        out << "    result = nc_add_pipe_" << id << "_entry(status);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Failed to add entry for pipe " << id
            << ": %s\", " << tmpl.get("error.to_string") << "(result));\n"
            << "        return result;\n"
            << "    }\n";
    }
    out << "\n    return DOCA_SUCCESS;\n}\n";
    return mlir::success();
}

} // anonymous namespace

namespace mlir {

mlir::LogicalResult generateDocaFlowCodeBF2(
    MLIRContext *ctx,
    llvm::StringRef inputDir,
    llvm::StringRef outDir,
    const std::map<int, int> &blockHwMap,
    const std::map<int, int> &armQueueMap) {

    // Load DOCA version template
    const char *ncRoot = std::getenv("NUTCRACKER_ROOT");
    std::string root = ncRoot ? ncRoot : ".";
    auto tmplOrErr = DocaFlowTemplate::loadDefault(root);
    if (!tmplOrErr) {
        llvm::errs() << "  ✗ Failed to load DOCA template: "
                     << llvm::toString(tmplOrErr.takeError()) << "\n"
                     << "    Set DOCA_TEMPLATE=<path> or NUTCRACKER_ROOT=<root>\n";
        return mlir::failure();
    }

    std::string outPath = (outDir + "/doca_flow_pipeline.c").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outPath, EC);
    if (EC) {
        llvm::errs() << "  ✗ Cannot open " << outPath
                     << ": " << EC.message() << "\n";
        return mlir::failure();
    }
    return doGenerate(ctx, inputDir, *tmplOrErr, blockHwMap, armQueueMap, out);
}

} // namespace mlir
