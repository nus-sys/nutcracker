//===- BF2DRMTTypes.cpp - BF3 NIC ASIC DRMT dialect types ------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"

#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTDialect.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.h"
#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h"

using namespace mlir;
using namespace mlir::bf2drmt;

#define GET_TYPEDEF_CLASSES
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.cpp.inc"

// ── Dialect-level parse/print hooks ──────────────────────────────────────────

mlir::Type mlir::bf2drmt::BF2DRMTDialect::parseType(
        mlir::DialectAsmParser &parser) const {
    mlir::SMLoc typeLoc = parser.getCurrentLocation();
    llvm::StringRef mnemonic;
    mlir::Type genType;
    mlir::OptionalParseResult parseResult =
        generatedTypeParser(parser, &mnemonic, genType);
    if (parseResult.has_value()) return genType;
    parser.emitError(typeLoc) << "unknown bf2drmt type: " << mnemonic;
    return mlir::Type();
}

void mlir::bf2drmt::BF2DRMTDialect::printType(
        mlir::Type type, mlir::DialectAsmPrinter &os) const {
    if (generatedTypePrinter(type, os).succeeded()) return;
    llvm::report_fatal_error("bf2drmt: missing type printer");
}

// ── Shared parse/print helpers ────────────────────────────────────────────────

static ParseResult parseFields(AsmParser &p, std::string &name,
                               SmallVectorImpl<vdrmt::FieldInfo> &parameters,
                               mlir::DictionaryAttr &annotations) {
    llvm::StringSet<> nameSet;
    mlir::NamedAttrList annList;
    bool hasDuplicateName = false;
    bool parsedName = false;
    auto parseResult =
        p.parseCommaSeparatedList(AsmParser::Delimiter::LessGreater, [&]() -> ParseResult {
            if (!parsedName) {
                if (p.parseKeywordOrString(&name) || p.parseOptionalAttrDict(annList))
                    return failure();
                parsedName = true;
                annotations = annList.getDictionary(p.getContext());
                return success();
            }
            std::string fieldName;
            mlir::Type fieldType;
            mlir::NamedAttrList fieldAnnotations;
            auto fieldLoc = p.getCurrentLocation();
            if (p.parseKeywordOrString(&fieldName) || p.parseColon() ||
                p.parseType(fieldType) || p.parseOptionalAttrDict(fieldAnnotations))
                return failure();
            if (!nameSet.insert(fieldName).second) {
                p.emitError(fieldLoc, "duplicate field name '" + fieldName + "'");
                hasDuplicateName = true;
            }
            parameters.emplace_back(StringAttr::get(p.getContext(), fieldName),
                                    fieldType,
                                    fieldAnnotations.getDictionary(p.getContext()));
            return success();
        });
    if (hasDuplicateName) return failure();
    return parseResult;
}

static void printFields(AsmPrinter &p, StringRef name,
                        ArrayRef<vdrmt::FieldInfo> fields,
                        mlir::DictionaryAttr annotations) {
    p << '<';
    p.printString(name);
    if (annotations && !annotations.empty()) {
        p << ' ';
        p.printAttributeWithoutType(annotations);
    }
    if (!fields.empty()) p << ", ";
    llvm::interleaveComma(fields, p, [&](const vdrmt::FieldInfo &field) {
        p.printKeywordOrString(field.name.getValue());
        p << ": " << field.type;
        if (field.annotations && !field.annotations.empty()) {
            p << " ";
            p.printAttributeWithoutType(field.annotations);
        }
    });
    p << ">";
}

// ── StructType ────────────────────────────────────────────────────────────────

Type StructType::parse(AsmParser &p) {
    llvm::SmallVector<vdrmt::FieldInfo, 4> parameters;
    std::string name;
    mlir::DictionaryAttr annotations;
    if (parseFields(p, name, parameters, annotations)) return {};
    return get(p.getContext(), name, parameters, annotations);
}

void StructType::print(AsmPrinter &p) const {
    printFields(p, getName(), getElements(), getAnnotations());
}

LogicalResult StructType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 StringRef name,
                                 ArrayRef<vdrmt::FieldInfo> elements,
                                 DictionaryAttr annotations) {
    llvm::StringSet<> fieldNames;
    for (const auto &field : elements)
        if (!fieldNames.insert(field.name.getValue()).second)
            return emitError() << "duplicate field name '" << field.name.getValue() << "'";
    return success();
}

// ── HeaderType ────────────────────────────────────────────────────────────────

Type HeaderType::parse(AsmParser &p) {
    llvm::SmallVector<vdrmt::FieldInfo, 4> parameters;
    std::string name;
    mlir::DictionaryAttr annotations;
    if (parseFields(p, name, parameters, annotations)) return {};
    return get(p.getContext(), name, parameters, annotations);
}

void HeaderType::print(AsmPrinter &p) const {
    printFields(p, getName(), getElements(), getAnnotations());
}

LogicalResult HeaderType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 StringRef name,
                                 ArrayRef<vdrmt::FieldInfo> elements,
                                 DictionaryAttr annotations) {
    llvm::StringSet<> fieldNames;
    for (const auto &field : elements)
        if (!fieldNames.insert(field.name.getValue()).second)
            return emitError() << "duplicate field name '" << field.name.getValue() << "'";
    return success();
}

// ── Dialect type registration ─────────────────────────────────────────────────

void mlir::bf2drmt::BF2DRMTDialect::registerTypes() {
    addTypes<
#define GET_TYPEDEF_LIST
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.cpp.inc"
    >();
}
