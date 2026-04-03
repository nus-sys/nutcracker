// DocaFlowTemplate.cpp
// Minimal TOML-subset parser and template engine for DOCA Flow code generation.
//
// Supported TOML subset:
//   [section]
//   [section.subsection]
//   ["section"."dotted.key"]      ← for match_fields entries
//   key = "single-line string"
//   key = """multiline string"""
//   key = 42                      ← integer (for width field)
//   # comments
//   Blank lines ignored.

#include "Pass/DocaFlowTemplate.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <cstdlib>
#include <sstream>

namespace mlir {

// ── TOML parser ──────────────────────────────────────────────────────────────

namespace {

/// Strip leading/trailing whitespace.
static std::string trim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Unescape \\n and \\t in a string value.
static std::string unescape(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'n') { out += '\n'; ++i; continue; }
            if (c == 't') { out += '\t'; ++i; continue; }
            if (c == '\\') { out += '\\'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

/// Parse a quoted key component: "foo.bar" → "foo.bar" (removes quotes).
/// Returns the content between quotes, or the bare word if not quoted.
static std::string parseKeyComponent(const std::string &s, size_t &pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos < s.size() && s[pos] == '"') {
        ++pos;
        std::string result;
        while (pos < s.size() && s[pos] != '"') result += s[pos++];
        if (pos < s.size()) ++pos; // consume closing "
        return result;
    }
    // Bare key: alphanumeric + _ + -
    std::string result;
    while (pos < s.size() && s[pos] != '.' && s[pos] != ']' &&
           s[pos] != ' ' && s[pos] != '\t')
        result += s[pos++];
    return result;
}

/// Parse a TOML table header "[a.b."c.d"]" → dotted path "a.b.c.d".
static std::string parseTableHeader(const std::string &line) {
    size_t start = line.find('[');
    size_t end   = line.rfind(']');
    if (start == std::string::npos || end == std::string::npos) return "";
    std::string inner = line.substr(start + 1, end - start - 1);
    // Parse dot-separated components (may be quoted)
    std::string path;
    size_t pos = 0;
    while (pos < inner.size()) {
        while (pos < inner.size() && (inner[pos] == ' ' || inner[pos] == '\t'))
            ++pos;
        if (pos >= inner.size()) break;
        std::string comp = parseKeyComponent(inner, pos);
        if (!path.empty()) path += '.';
        path += comp;
        while (pos < inner.size() && (inner[pos] == ' ' || inner[pos] == '\t'))
            ++pos;
        if (pos < inner.size() && inner[pos] == '.') ++pos;
    }
    return path;
}


} // anonymous namespace

// ── DocaFlowTemplate::loadFromFile ───────────────────────────────────────────

llvm::Expected<DocaFlowTemplate>
DocaFlowTemplate::loadFromFile(llvm::StringRef path) {
    auto bufOrErr = llvm::MemoryBuffer::getFile(path);
    if (!bufOrErr)
        return llvm::createStringError(bufOrErr.getError(),
            "cannot open DOCA template file: %s", path.data());

    DocaFlowTemplate tmpl;
    std::istringstream stream((*bufOrErr)->getBuffer().str());
    std::string line;
    std::string currentSection; // current [section] path

    while (std::getline(stream, line)) {
        // Strip inline comments and trailing whitespace
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos) line = line.substr(0, hashPos);
        line = trim(line);
        if (line.empty()) continue;

        // Table header
        if (line.front() == '[') {
            currentSection = parseTableHeader(line);
            continue;
        }

        // Key = value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        // Remove quotes from key if present
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"')
            key = key.substr(1, key.size() - 2);

        // Parse value
        std::string parsedValue;
        if (value.size() >= 3 && value.substr(0, 3) == "\"\"\"") {
            // Multiline string — may span further lines
            std::string rest = value.substr(3);
            // Strip immediate leading newline
            if (!rest.empty() && rest[0] == '\n') rest = rest.substr(1);
            else if (rest.size() >= 2 && rest[0]=='\r' && rest[1]=='\n')
                rest = rest.substr(2);

            std::string buf = rest;
            while (true) {
                size_t close = buf.find("\"\"\"");
                if (close != std::string::npos) {
                    parsedValue += buf.substr(0, close);
                    break;
                }
                parsedValue += buf + "\n";
                std::string nextLine;
                if (!std::getline(stream, nextLine)) break;
                buf = nextLine;
            }
        } else if (!value.empty() && value.front() == '"') {
            // Single-quoted string
            parsedValue = value.substr(1);
            size_t closeQ = parsedValue.rfind('"');
            if (closeQ != std::string::npos)
                parsedValue = parsedValue.substr(0, closeQ);
            parsedValue = unescape(parsedValue);
        } else {
            // Integer or unquoted value → store as-is
            parsedValue = value;
        }

        // Build full dot-path key
        std::string fullKey = currentSection.empty()
            ? key
            : currentSection + "." + key;

        tmpl.values_[fullKey] = parsedValue;

        // If this is a match_fields entry, populate matchFields_
        // Section looks like: match_fields.ipv4_t.src_addr
        if (currentSection.rfind("match_fields.", 0) == 0) {
            // field id = everything after "match_fields."
            std::string fieldId = currentSection.substr(std::string("match_fields.").size());
            MatchFieldInfo &mfi = tmpl.matchFields_[fieldId];
            if (key == "match_member") mfi.matchMember = parsedValue;
            else if (key == "field_string") mfi.fieldString = parsedValue;
            else if (key == "setup") mfi.extraSetup = parsedValue;
            else if (key == "width") mfi.widthBits = (unsigned)std::stoi(parsedValue);
        }
    }

    tmpl.valid_ = true;
    return tmpl;
}

// ── DocaFlowTemplate::loadDefault ────────────────────────────────────────────

llvm::Expected<DocaFlowTemplate>
DocaFlowTemplate::loadDefault(llvm::StringRef nutcrackerRoot) {
    // 1. Check DOCA_TEMPLATE env var
    if (const char *envPath = std::getenv("DOCA_TEMPLATE"))
        return loadFromFile(envPath);

    // 2. Default: $NUTCRACKER_ROOT/templates/doca-2.9.toml
    llvm::SmallString<256> defaultPath(nutcrackerRoot);
    llvm::sys::path::append(defaultPath, "templates", "doca-2.9.toml");
    return loadFromFile(defaultPath);
}

// ── DocaFlowTemplate::get ─────────────────────────────────────────────────────

std::string DocaFlowTemplate::get(llvm::StringRef key) const {
    auto it = values_.find(key.str());
    if (it != values_.end()) return it->second;
    return "";
}

// ── DocaFlowTemplate::substitute ────────────────────────────────────────────

std::string DocaFlowTemplate::substitute(
        const std::string &tmpl,
        std::initializer_list<std::pair<std::string,std::string>> vars) {
    std::string result = tmpl;
    for (auto &[placeholder, value] : vars) {
        std::string token = "{" + placeholder + "}";
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos) {
            result.replace(pos, token.size(), value);
            pos += value.size();
        }
    }
    return result;
}

// ── DocaFlowTemplate::render ─────────────────────────────────────────────────

std::string DocaFlowTemplate::render(
        llvm::StringRef key,
        std::initializer_list<std::pair<std::string,std::string>> vars) const {
    auto it = values_.find(key.str());
    if (it == values_.end())
        return "/* [doca-template: missing key '" + key.str() + "'] */";
    return substitute(it->second, vars);
}

// ── DocaFlowTemplate::lookupMatchField ───────────────────────────────────────

const DocaFlowTemplate::MatchFieldInfo *
DocaFlowTemplate::lookupMatchField(llvm::StringRef hdrType,
                                   llvm::StringRef fieldName) const {
    std::string key = hdrType.str() + "." + fieldName.str();
    auto it = matchFields_.find(key);
    if (it != matchFields_.end()) return &it->second;
    return nullptr;
}

// ── DocaFlowTemplate::lookupEnum ─────────────────────────────────────────────

std::string DocaFlowTemplate::lookupEnum(llvm::StringRef enumName,
                                          llvm::StringRef key) const {
    std::string fullKey = "enums." + enumName.str() + "." + key.str();
    return get(fullKey);
}

} // namespace mlir
