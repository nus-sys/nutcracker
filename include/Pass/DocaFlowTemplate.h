//===- DocaFlowTemplate.h - DOCA Flow version-specific code templates ----===//
//
// Loads a TOML template file describing DOCA Flow API calls for a specific
// DOCA version. The codegen uses this to emit version-appropriate C code
// without hardcoding any DOCA API strings.
//
// Template files live in $NUTCRACKER_ROOT/templates/doca-<version>.toml.
// Select via --doca-template=<path> or the DOCA_TEMPLATE env var.
//===----------------------------------------------------------------------===//

#ifndef NUTCRACKER_PASS_DOCAFLOWTEMPLATE_H
#define NUTCRACKER_PASS_DOCAFLOWTEMPLATE_H

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <map>
#include <string>

namespace mlir {

/// Holds all version-specific DOCA API strings loaded from a .toml template.
class DocaFlowTemplate {
public:
    // ── Match field descriptor ───────────────────────────────────────────────
    // Loaded from [match_fields."hdrType.fieldName"] entries.
    struct MatchFieldInfo {
        std::string matchMember; ///< e.g. "outer.ip4.src_ip"  (struct access)
        std::string fieldString; ///< e.g. "outer.ipv4.src_ip" (DOCA string API)
        std::string extraSetup;  ///< extra lines emitted once before match fields
        unsigned    widthBits = 32;
    };

    // ── Factory ──────────────────────────────────────────────────────────────

    /// Load a template file from path. Returns error if the file is missing or
    /// contains unknown keys for required sections.
    static llvm::Expected<DocaFlowTemplate> loadFromFile(llvm::StringRef path);

    /// Load the default template: looks for DOCA_TEMPLATE env var, then
    /// $NUTCRACKER_ROOT/templates/doca-2.9.toml.
    static llvm::Expected<DocaFlowTemplate> loadDefault(llvm::StringRef nutcrackerRoot);

    // ── Template rendering ───────────────────────────────────────────────────

    /// Render template at dot-path key (e.g. "pipe.cfg_create") substituting
    /// all {placeholder} occurrences. Returns the key verbatim (wrapped in a
    /// comment) if not found, so missing templates produce visible but
    /// compilable output.
    std::string render(llvm::StringRef key,
                       std::initializer_list<std::pair<std::string,std::string>> vars = {}) const;

    // ── Direct accessors ─────────────────────────────────────────────────────

    /// Raw string value at dot-path key, empty string if absent.
    std::string get(llvm::StringRef key) const;

    /// Match field lookup by "hdrType.fieldName" key.
    const MatchFieldInfo *lookupMatchField(llvm::StringRef hdrType,
                                           llvm::StringRef fieldName) const;

    /// Enum value lookup (e.g. lookupEnum("pipe_type", "basic")).
    std::string lookupEnum(llvm::StringRef enumName, llvm::StringRef key) const;

    bool valid() const { return valid_; }

private:
    // Flat map of "section.key" → value (dot-separated path).
    std::map<std::string, std::string> values_;
    // Match fields: "hdrType.fieldName" → MatchFieldInfo.
    std::map<std::string, MatchFieldInfo> matchFields_;
    bool valid_ = false;

    static std::string substitute(const std::string &tmpl,
                                  std::initializer_list<std::pair<std::string,std::string>> vars);
};

} // namespace mlir

#endif // NUTCRACKER_PASS_DOCAFLOWTEMPLATE_H
