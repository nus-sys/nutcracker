// ============================================================================
// File: lib/Pass/BF3DRMTToDocaFlow/DocaFlowCodegen.cpp
//
// DOCA Flow C codegen from bf3drmt dialect.
//
// Reads all bf3drmt.mlir files produced by VDRMTToBF3DRMTPass from inputDir
// and emits inputDir/doca_flow_pipeline.c — a self-contained C source
// implementing the full match-action pipeline as DOCA Flow pipes.
//
// Mapping:
//   bf3drmt.pipe type(basic)   → doca_flow_pipe_cfg + DOCA_FLOW_PIPE_BASIC
//   bf3drmt.table_key          → match struct (field masks)
//   bf3drmt.pipe_action @_hit  → fwd.next_pipe = g_pipes[N]
//   bf3drmt.pipe_action @_miss → fwd_miss.next_pipe = g_pipes[N]
//   bf3drmt.next -1            → fwd.type = DOCA_FLOW_FWD_PORT (egress)
//   bf3drmt.assign (FieldCopy) → DOCA_FLOW_ACTION_COPY action_desc
//   bf3drmt.assign (Constant)  → actions.meta.u32[N] set per entry
// ============================================================================

#include "Pass/BF3DRMTToDocaFlowPass.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTTypes.h"

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

/// Walk a Value backwards through bf3drmt.struct_extract_ref ops.
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
        auto serOp = mlir::dyn_cast<bf3drmt::StructExtractRefOp>(defOp);
        if (!serOp) return path;

        std::string name = "field" + std::to_string(serOp.getFieldIndex());
        mlir::Type leafObjType;
        if (auto refTy = mlir::dyn_cast<bf3drmt::ReferenceType>(
                serOp.getInput().getType())) {
            auto objTy = refTy.getObjectType();
            auto nameAt = [&](auto containerTy) -> std::string {
                auto elems = containerTy.getElements();
                int idx = serOp.getFieldIndex();
                if (idx >= 0 && (size_t)idx < elems.size())
                    return elems[idx].name.getValue().str();
                return "field" + std::to_string(idx);
            };
            if (auto sTy = mlir::dyn_cast<bf3drmt::StructType>(objTy))
                name = nameAt(sTy);
            else if (auto hTy = mlir::dyn_cast<bf3drmt::HeaderType>(objTy))
                name = nameAt(hTy);
        }
        if (auto resRef = mlir::dyn_cast<bf3drmt::ReferenceType>(
                serOp.getResult().getType()))
            leafObjType = resRef.getObjectType();
        path.steps.push_back({serOp.getFieldIndex(), name, leafObjType});
        val = serOp.getInput();
    }
}

// ── DOCA field lookup ────────────────────────────────────────────────────────

struct DocaFieldInfo {
    std::string matchMember; ///< e.g. "outer.ip4.next_proto"
    std::string fieldString; ///< e.g. "outer.ipv4.next_proto"
    std::string extraSetup;  ///< extra match lines (l3/l4 type guards)
    unsigned    widthBits;
};

static DocaFieldInfo lookupDocaField(const std::string &hdrType,
                                     const std::string &fieldName) {
    using K = std::pair<std::string, std::string>;
    static const std::map<K, DocaFieldInfo> kTable = {
        {{"ipv4_t","protocol"}, {"outer.ip4.next_proto", "outer.ipv4.next_proto",
          "match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;\n    "
          "match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;", 8}},
        {{"ipv4_t","src_addr"}, {"outer.ip4.src_ip", "outer.ipv4.src_ip",
          "match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;\n    "
          "match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;", 32}},
        {{"ipv4_t","dst_addr"}, {"outer.ip4.dst_ip", "outer.ipv4.dst_ip",
          "match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;\n    "
          "match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;", 32}},
        {{"ipv4_t","ttl"},      {"outer.ip4.ttl", "outer.ipv4.ttl",
          "match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;\n    "
          "match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;", 8}},
        {{"tcp_t","src_port"},  {"outer.tcp.l4_port.src_port", "outer.tcp.src_port",
          "match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;\n    "
          "match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;", 16}},
        {{"tcp_t","dst_port"},  {"outer.tcp.l4_port.dst_port", "outer.tcp.dst_port",
          "match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;\n    "
          "match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;", 16}},
        {{"udp_t","src_port"},  {"outer.udp.l4_port.src_port", "outer.udp.src_port",
          "match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;\n    "
          "match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;", 16}},
        {{"udp_t","dst_port"},  {"outer.udp.l4_port.dst_port", "outer.udp.dst_port",
          "match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;\n    "
          "match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;", 16}},
    };
    auto it = kTable.find({hdrType, fieldName});
    if (it != kTable.end()) return it->second;
    return {"/* unknown:" + hdrType + "." + fieldName + " */",
            "/* unknown:" + hdrType + "." + fieldName + " */", "", 32};
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

    if (auto constOp = mlir::dyn_cast<bf3drmt::ConstantOp>(defOp)) {
        if (auto ia = mlir::dyn_cast<mlir::IntegerAttr>(constOp.getValue())) {
            av.kind = ActionValue::Kind::Constant;
            av.constVal = ia.getInt();
        }
        return av;
    }

    if (auto readOp = mlir::dyn_cast<bf3drmt::ReadOp>(defOp)) {
        av.kind = ActionValue::Kind::FieldCopy;
        av.srcRef = resolveRefChain(readOp.getRef());
        if (auto iT = mlir::dyn_cast<mlir::IntegerType>(readOp.getResult().getType()))
            av.copyWidth = iT.getWidth();
        av.copyHighBit = (int)av.copyWidth - 1;
        return av;
    }

    if (auto sliceOp = mlir::dyn_cast<bf3drmt::SliceOp>(defOp)) {
        if (auto *rDef = sliceOp.getInput().getDefiningOp())
            if (auto readOp = mlir::dyn_cast<bf3drmt::ReadOp>(rDef)) {
                av.kind = ActionValue::Kind::FieldCopyMasked;
                av.srcRef = resolveRefChain(readOp.getRef());
                av.copyLowBit  = (int)sliceOp.getLowBit();
                av.copyHighBit = (int)sliceOp.getHighBit();
                av.copyWidth   = av.copyHighBit - av.copyLowBit + 1;
            }
        return av;
    }

    if (auto binOp = mlir::dyn_cast<bf3drmt::BinOp>(defOp)) {
        if (binOp.getKind() == bf3drmt::BF3DRMTBinOpKind::And) {
            mlir::Value readVal, maskVal;
            if (mlir::dyn_cast_or_null<bf3drmt::ConstantOp>(
                    binOp.getRhs().getDefiningOp()))
                { readVal = binOp.getLhs(); maskVal = binOp.getRhs(); }
            else if (mlir::dyn_cast_or_null<bf3drmt::ConstantOp>(
                    binOp.getLhs().getDefiningOp()))
                { readVal = binOp.getRhs(); maskVal = binOp.getLhs(); }

            if (readVal && maskVal) {
                auto maskConst =
                    mlir::cast<bf3drmt::ConstantOp>(maskVal.getDefiningOp());
                int64_t mask =
                    mlir::cast<mlir::IntegerAttr>(maskConst.getValue()).getInt();
                int lo = 0;
                while (lo < 63 && !(mask >> lo & 1)) lo++;
                int hi = lo;
                while (hi < 62 && (mask >> (hi + 1) & 1)) hi++;
                if (auto *rDef = readVal.getDefiningOp())
                    if (auto readOp = mlir::dyn_cast<bf3drmt::ReadOp>(rDef)) {
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
/// For header fields: looked up in the DOCA field table.
static std::string refPathToDocaFieldStr(const RefPath &rp) {
    if (!rp.valid() || rp.steps.empty()) return "/* unresolved */";
    if (rp.argIdx == 1)
        return "meta.data";
    if (rp.argIdx == 0 && rp.steps.size() >= 2) {
        std::string hdrType;
        if (auto hTy = mlir::dyn_cast<bf3drmt::HeaderType>(rp.steps[0].objectType))
            hdrType = hTy.getName().str();
        return lookupDocaField(hdrType, rp.steps[1].fieldName).fieldString;
    }
    return "/* unresolved */";
}

// ── Code generation ───────────────────────────────────────────────────────────

mlir::LogicalResult doGenerate(MLIRContext *ctx, llvm::StringRef inputDir,
                               const std::map<int, int> &blockHwMap,
                               const std::map<int, int> &armQueueMap,
                               llvm::raw_ostream &out) {
    struct PipeRecord {
        int blockId;
        mlir::OwningOpRef<mlir::ModuleOp> module;
        bf3drmt::PipeOp pipeOp;
    };
    std::vector<PipeRecord> pipes;

    std::error_code ec;
    for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
         dir != end && !ec; dir.increment(ec)) {
        auto path = dir->path();
        if (!llvm::sys::fs::is_directory(path)) continue;
        std::string dirName = llvm::sys::path::filename(path).str();
        if (dirName.find("block") != 0) continue;

        std::string bf3File = path + "/bf3drmt.mlir";
        if (!llvm::sys::fs::exists(bf3File)) continue;

        auto mod = parseSourceFile<ModuleOp>(bf3File, ctx);
        if (!mod) continue;

        for (auto pipeOp : mod->getOps<bf3drmt::PipeOp>()) {
            std::string sym = pipeOp.getSymName().str();
            std::string digits;
            for (char c : sym) if (std::isdigit(c)) digits += c;
            int id = digits.empty() ? -1 : std::stoi(digits);
            pipes.push_back({id, std::move(mod), pipeOp});
            break;
        }
    }
    if (pipes.empty()) {
        llvm::errs() << "  ✗ No bf3drmt.mlir files to generate from\n";
        return mlir::failure();
    }
    llvm::sort(pipes, [](const PipeRecord &a, const PipeRecord &b) {
        return a.blockId < b.blockId;
    });
    int nPipes = (int)pipes.size();

    // g_pipes must be indexed by block ID (not pipe count).
    // Find the max DRMT block ID so the array covers all valid indices.
    int maxDrmtBlockId = -1;
    for (auto &pr : pipes)
        if (pr.blockId > maxDrmtBlockId) maxDrmtBlockId = pr.blockId;
    int gPipesSize = maxDrmtBlockId + 1;

    // Successor map: blockId → [successor blockIds] (DRMT-only successors for topo sort)
    std::map<int, std::vector<int>> successors;
    for (auto &pr : pipes) {
        pr.pipeOp->walk([&](bf3drmt::NextOp nextOp) {
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
        << " * Generated by nutcracker BF3DRMTToDocaFlow codegen\n"
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

    // ── Per-pipe functions ─────────────────────────────────────────────────
    for (auto &pr : pipes) {
        int id = pr.blockId;
        bf3drmt::PipeOp pipeOp = pr.pipeOp;
        std::string pipeName = "NC_BLOCK" + std::to_string(id);

        std::string pipeTypeStr = "DOCA_FLOW_PIPE_BASIC";
        switch (pipeOp.getPipeType()) {
            case bf3drmt::BF3DRMTPipeType::Acl:     pipeTypeStr = "DOCA_FLOW_PIPE_ACL";     break;
            case bf3drmt::BF3DRMTPipeType::Control: pipeTypeStr = "DOCA_FLOW_PIPE_CONTROL"; break;
            case bf3drmt::BF3DRMTPipeType::Hash:    pipeTypeStr = "DOCA_FLOW_PIPE_HASH";    break;
            case bf3drmt::BF3DRMTPipeType::Lpm:     pipeTypeStr = "DOCA_FLOW_PIPE_LPM";     break;
            default: break;
        }

        struct MatchField  { DocaFieldInfo doca; unsigned widthBits; };
        struct AssignSlot  { RefPath dst; ActionValue value; };
        struct ActionRegion {
            std::string actionSym;
            int nextBlock;
            std::vector<AssignSlot> assigns;
        };

        std::vector<MatchField>   matchFields;
        std::vector<ActionRegion> actionRegions;
        bool        hasTableKey = false;
        std::string extraMatchSetup;

        auto &body = pipeOp.getBody().front();
        for (auto &op : body) {
            if (auto tkOp = mlir::dyn_cast<bf3drmt::TableKeyOp>(&op)) {
                hasTableKey = true;
                tkOp.getBody().front().walk([&](bf3drmt::MatchKeyOp mkOp) {
                    auto *kDef = mkOp.getKey().getDefiningOp();
                    if (!kDef) return;
                    mlir::Value refToRead;
                    if (auto rd = mlir::dyn_cast<bf3drmt::ReadOp>(kDef))
                        refToRead = rd.getRef();
                    if (!refToRead) return;
                    RefPath rp = resolveRefChain(refToRead);
                    if (!rp.valid() || rp.argIdx != 0 || rp.steps.size() < 2)
                        return;
                    std::string hdrType;
                    if (auto hTy = mlir::dyn_cast<bf3drmt::HeaderType>(
                            rp.steps[0].objectType))
                        hdrType = hTy.getName().str();
                    auto doca = lookupDocaField(hdrType, rp.steps[1].fieldName);
                    if (!doca.extraSetup.empty() &&
                        extraMatchSetup.find(doca.extraSetup) == std::string::npos)
                        extraMatchSetup += doca.extraSetup + "\n    ";
                    matchFields.push_back({doca, doca.widthBits});
                });
                continue;
            }
            if (auto paOp = mlir::dyn_cast<bf3drmt::PipeActionOp>(&op)) {
                ActionRegion ar;
                ar.actionSym = paOp.getAction().getRootReference().str();
                ar.nextBlock = -1;
                for (auto &aop : paOp.getBody().front()) {
                    if (auto nextOp = mlir::dyn_cast<bf3drmt::NextOp>(&aop))
                        ar.nextBlock = (int)nextOp.getSuccessor();
                    else if (auto assignOp = mlir::dyn_cast<bf3drmt::AssignOp>(&aop))
                        ar.assigns.push_back({resolveRefChain(assignOp.getRef()),
                                              resolveActionValue(assignOp.getValue())});
                }
                actionRegions.push_back(std::move(ar));
                continue;
            }
        }

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

        // ── nc_create_pipe_N(port, port_id) ──────────────────────────────
        out << "/* ── Block " << id << ": " << pipeName
            << " ──────────────────────────── */\n"
            << "static doca_error_t nc_create_pipe_" << id
            << "(struct doca_flow_port *port, int port_id)\n{\n"
            << "    struct doca_flow_match match;\n"
            << "    struct doca_flow_actions actions,"
               " *actions_arr[NB_ACTIONS_ARR] = {&actions};\n";
        if (hasActionDescs) {
            out << "    struct doca_flow_action_desc desc_array[" << nDescs << "];\n"
                << "    struct doca_flow_action_descs descs,"
                   " *descs_arr[NB_ACTIONS_ARR] = {&descs};\n";
        }
        out << "    struct doca_flow_fwd fwd;\n";
        if (hasMiss || hasTableKey)
            out << "    struct doca_flow_fwd fwd_miss;\n";
        out << "    struct doca_flow_pipe_cfg *pipe_cfg;\n"
            << "    doca_error_t result;\n\n"
            << "    memset(&match,   0, sizeof(match));\n"
            << "    memset(&actions, 0, sizeof(actions));\n"
            << "    memset(&fwd,     0, sizeof(fwd));\n";
        if (hasMiss || hasTableKey)
            out << "    memset(&fwd_miss, 0, sizeof(fwd_miss));\n";
        if (hasActionDescs)
            out << "    memset(desc_array, 0, sizeof(desc_array));\n"
                << "    memset(&descs,     0, sizeof(descs));\n";
        out << "\n";

        // Match fields
        if (!matchFields.empty()) {
            out << "    /* Match key fields */\n";
            if (!extraMatchSetup.empty())
                out << "    " << extraMatchSetup << "\n";
            for (auto &mf : matchFields)
                if (!mf.doca.matchMember.empty())
                    out << "    match." << mf.doca.matchMember
                        << " = " << maskForWidth(mf.widthBits) << ";\n";
            out << "\n";
        }

        // Action descriptors (field-to-field copy)
        if (hasActionDescs) {
            out << "    /* Action descriptors: field-to-field copy */\n";
            int di = 0;
            for (auto &ar : actionRegions) {
                for (auto &sl : ar.assigns) {
                    if (sl.value.kind != ActionValue::Kind::FieldCopy &&
                        sl.value.kind != ActionValue::Kind::FieldCopyMasked)
                        continue;
                    std::string srcStr = refPathToDocaFieldStr(sl.value.srcRef);
                    std::string dstStr = refPathToDocaFieldStr(sl.dst);
                    std::string dstBitOff = "0";
                    if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty())
                        dstBitOff = "META_U32_BIT_OFFSET("
                                    + std::to_string(sl.dst.steps[0].fieldIdx) + ")";
                    out << "    desc_array[" << di << "].type = DOCA_FLOW_ACTION_COPY;\n"
                        << "    desc_array[" << di << "].field_op.src.field_string = \""
                        << srcStr << "\";\n"
                        << "    desc_array[" << di << "].field_op.src.bit_offset = "
                        << sl.value.copyLowBit << ";\n"
                        << "    desc_array[" << di << "].field_op.dst.field_string = \""
                        << dstStr << "\";\n"
                        << "    desc_array[" << di << "].field_op.dst.bit_offset = "
                        << dstBitOff << ";\n"
                        << "    desc_array[" << di << "].field_op.width = "
                        << sl.value.copyWidth << ";\n";
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
                    out << "    actions.meta.u32[" << sl.dst.steps[0].fieldIdx
                        << "] = UINT32_MAX; /* mask: constant set per entry */\n";
            }

        // Pipe creation
        out << "\n    result = doca_flow_pipe_cfg_create(&pipe_cfg, port);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Block" << id
            << ": pipe_cfg_create failed: %s\", doca_error_get_descr(result));\n"
            << "        return result;\n"
            << "    }\n\n"
            << "    result = doca_flow_pipe_cfg_set_name(pipe_cfg, \"" << pipeName << "\");\n"
            << "    if (result != DOCA_SUCCESS) goto destroy;\n"
            << "    result = doca_flow_pipe_cfg_set_type(pipe_cfg, " << pipeTypeStr << ");\n"
            << "    if (result != DOCA_SUCCESS) goto destroy;\n"
            << "    result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, "
            << (hasTableKey ? "true" : "false") << ");\n"
            << "    if (result != DOCA_SUCCESS) goto destroy;\n\n"
            << "    result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);\n"
            << "    if (result != DOCA_SUCCESS) goto destroy;\n\n";
        if (hasActionDescs)
            out << "    result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr,"
                   " NULL, descs_arr, NB_ACTIONS_ARR);\n";
        else
            out << "    result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr,"
                   " NULL, NULL, NB_ACTIONS_ARR);\n";
        out << "    if (result != DOCA_SUCCESS) goto destroy;\n\n";

        // Fwd / fwd_miss
        int hitNext = -1, missNext = -1, defaultNext = -1;
        for (auto &ar : actionRegions) {
            if (ar.actionSym == "_hit")      hitNext     = ar.nextBlock;
            else if (ar.actionSym == "_miss")    missNext    = ar.nextBlock;
            else if (ar.actionSym == "_default") defaultNext = ar.nextBlock;
        }
        int mainNext = (hitNext >= 0) ? hitNext : defaultNext;

        auto emitFwd = [&](const std::string &var, int nextId) {
            if (nextId == -1) {
                // Egress: send to the other port (port_id ^ 1 is the DOCA convention
                // for VNF/hairpin setups where port 0 and port 1 are paired).
                out << "    " << var << ".type = DOCA_FLOW_FWD_PORT;\n"
                    << "    " << var << ".port_id = port_id ^ 1;\n";
                return;
            }
            auto hwIt = blockHwMap.find(nextId);
            int hw = (hwIt != blockHwMap.end()) ? hwIt->second : 0; // default DRMT
            if (hw == 2) { // ARM: RSS to the entrypoint's dedicated queue
                auto qIt = armQueueMap.find(nextId);
                int qidx = (qIt != armQueueMap.end()) ? qIt->second : 0;
                out << "    " << var << ".type = DOCA_FLOW_FWD_RSS;\n"
                    << "    " << var << ".rss_queues = (uint16_t[]){ " << qidx
                    << " }; /* ARM block " << nextId << " queue */\n"
                    << "    " << var << ".num_of_queues = 1;\n"
                    << "    " << var << ".rss_outer_flags = DOCA_FLOW_RSS_IPV4"
                    << " | DOCA_FLOW_RSS_UDP | DOCA_FLOW_RSS_TCP;\n";
            } else if (hw == 1) { // DPA: RSS to dedicated queue NC_DPA_QUEUE_BASE+id
                out << "    " << var << ".type = DOCA_FLOW_FWD_RSS;\n"
                    << "    " << var << ".rss_queues = (uint16_t[]){ (uint16_t)"
                    << "(NC_DPA_QUEUE_BASE + " << nextId << ") };"
                    << " /* DPA block " << nextId << " */\n"
                    << "    " << var << ".num_of_queues = 1;\n"
                    << "    " << var << ".rss_outer_flags = DOCA_FLOW_RSS_IPV4"
                    << " | DOCA_FLOW_RSS_UDP | DOCA_FLOW_RSS_TCP;\n";
            } else { // DRMT: chain to next DRMT pipe
                out << "    " << var << ".type = DOCA_FLOW_FWD_PIPE;\n"
                    << "    " << var << ".next_pipe = g_pipes[" << nextId
                    << "]; /* block " << nextId << " */\n";
            }
        };

        emitFwd("fwd", mainNext);
        if (hasMiss || (hasTableKey && missNext >= 0)) {
            out << "\n";
            emitFwd("fwd_miss", missNext);
        }

        out << "\n    result = doca_flow_pipe_create(pipe_cfg, &fwd, "
            << ((hasMiss || (hasTableKey && missNext >= 0)) ? "&fwd_miss" : "NULL")
            << ", &g_pipes[" << id << "]);\n"
            << "destroy:\n"
            << "    doca_flow_pipe_cfg_destroy(pipe_cfg);\n"
            << "    return result;\n"
            << "}\n\n";

        // ── nc_add_pipe_N_entry() ──────────────────────────────────────────
        out << "static doca_error_t nc_add_pipe_" << id
            << "_entry(struct entries_status *status)\n{\n"
            << "    struct doca_flow_match match;\n"
            << "    struct doca_flow_actions actions;\n"
            << "    struct doca_flow_pipe_entry *entry;\n"
            << "    doca_error_t result;\n\n"
            << "    memset(&match,   0, sizeof(match));\n"
            << "    memset(&actions, 0, sizeof(actions));\n\n";

        bool hasEntryActions = false;
        for (auto &ar : actionRegions)
            for (auto &sl : ar.assigns) {
                if (sl.value.kind != ActionValue::Kind::Constant) continue;
                if (sl.dst.valid() && sl.dst.argIdx == 1 && !sl.dst.steps.empty()) {
                    out << "    actions.meta.u32[" << sl.dst.steps[0].fieldIdx
                        << "] = " << sl.value.constVal << ";\n";
                    hasEntryActions = true;
                }
            }

        if (!hasEntryActions && matchFields.empty())
            out << "    /* No match key or constant actions for this block */\n";
        else if (!matchFields.empty())
            out << "    /* TODO: set match fields to runtime key values */\n";

        out << "\n    actions.action_idx = 0;\n"
            << "    result = doca_flow_pipe_add_entry(0, g_pipes[" << id
            << "], &match, &actions, NULL, NULL,\n"
            << "                                     DOCA_FLOW_NO_WAIT, status, &entry);\n"
            << "    return result;\n"
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
        << "    doca_error_t result;\n\n"
        << "    /* Create pipes in reverse topological order (leaves first) */\n";
    for (int id : createOrder) {
        out << "    result = nc_create_pipe_" << id << "(port, port_id);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Failed to create pipe " << id
            << ": %s\", doca_error_get_descr(result));\n"
            << "        return result;\n"
            << "    }\n";
    }
    out << "\n    /* Add default entries */\n";
    for (int id : createOrder) {
        out << "    result = nc_add_pipe_" << id << "_entry(status);\n"
            << "    if (result != DOCA_SUCCESS) {\n"
            << "        DOCA_LOG_ERR(\"Failed to add entry for pipe " << id
            << ": %s\", doca_error_get_descr(result));\n"
            << "        return result;\n"
            << "    }\n";
    }
    out << "\n    return DOCA_SUCCESS;\n}\n";
    return mlir::success();
}

} // anonymous namespace

namespace mlir {

mlir::LogicalResult generateDocaFlowCode(
    MLIRContext *ctx,
    llvm::StringRef inputDir,
    llvm::StringRef outDir,
    const std::map<int, int> &blockHwMap,
    const std::map<int, int> &armQueueMap) {
    std::string outPath = (outDir + "/doca_flow_pipeline.c").str();
    std::error_code EC;
    llvm::raw_fd_ostream out(outPath, EC);
    if (EC) {
        llvm::errs() << "  ✗ Cannot open " << outPath
                     << ": " << EC.message() << "\n";
        return mlir::failure();
    }
    return doGenerate(ctx, inputDir, blockHwMap, armQueueMap, out);
}

} // namespace mlir
