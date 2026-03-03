//===- vDRMTTypes.cpp - Virtual DRMT types ---------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Support/LLVM.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOpsEnums.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

using namespace mlir;
using namespace mlir::vdrmt;

static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params,
                                       mlir::Type &optionalResultType);
static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params);

static void printFuncType(mlir::AsmPrinter &p, mlir::ArrayRef<mlir::Type> params,
                          mlir::Type optionalResultType = {});
                          
#define GET_TYPEDEF_CLASSES
#include "Dialect/vDRMT/IR/vDRMTTypes.cpp.inc"

Type vDRMTDialect::parseType(mlir::DialectAsmParser &parser) const {
    SMLoc typeLoc = parser.getCurrentLocation();
    StringRef mnemonic;
    Type genType;

    // Try to parse as a tablegen'd type.
    OptionalParseResult parseResult = generatedTypeParser(parser, &mnemonic, genType);
    if (parseResult.has_value()) return genType;

    // No additional custom types to parse
    parser.emitError(typeLoc) << "unknown DOCAFlow type: " << mnemonic;
    return Type();
}

void vDRMTDialect::printType(mlir::Type type, mlir::DialectAsmPrinter &os) const {
    return;
}

//===----------------------------------------------------------------------===//
// FuncType
//===----------------------------------------------------------------------===//
static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params) {
    mlir::Type placeholder;
    return parseFuncType(p, params, placeholder);
}

static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params,
                                       mlir::Type &optionalReturnType) {
    // Parse return type, if any
    if (succeeded(p.parseOptionalLParen())) {
        // If we have already a '(', the function has no return type
        optionalReturnType = {};
    } else {
        mlir::Type type;
        if (p.parseType(type)) return mlir::failure();
        if (mlir::isa<VoidType>(type))
            // An explicit !p4hir.void means also no return type.
            optionalReturnType = {};
        else
            // Otherwise use the actual type.
            optionalReturnType = type;
        if (p.parseLParen()) return mlir::failure();
    }

    // `(` `)`
    if (succeeded(p.parseOptionalRParen())) return mlir::success();

    if (p.parseCommaSeparatedList([&]() -> ParseResult {
            mlir::Type type;
            if (p.parseType(type)) return mlir::failure();
            params.push_back(type);
            return mlir::success();
        }))
        return mlir::failure();

    return p.parseRParen();
}

static void printFuncType(mlir::AsmPrinter &p, mlir::ArrayRef<mlir::Type> params,
                          mlir::Type optionalReturnType) {
    if (optionalReturnType) p << optionalReturnType << ' ';
    p << '(';
    llvm::interleaveComma(params, p, [&p](mlir::Type type) { p.printType(type); });
    p << ')';
}

// Whether the function returns void
bool FuncType::isVoid() const {
    auto rt = static_cast<detail::FuncTypeStorage *>(getImpl())->optionalReturnType;
    assert(!rt || !mlir::isa<VoidType>(rt) &&
                      "The return type for a function returning void should be empty "
                      "instead of a real !p4hir.void");
    return !rt;
}

// Return the actual return type or an explicit !vdrmt.void if the function does
// not return anything
mlir::Type FuncType::getReturnType() const {
    if (isVoid()) return vdrmt::VoidType::get(getContext());
    return static_cast<detail::FuncTypeStorage *>(getImpl())->optionalReturnType;
}

// Returns the result type of the function as an ArrayRef
llvm::ArrayRef<mlir::Type> FuncType::getReturnTypes() const {
    if (isVoid()) return {};
    return getReturnType();
}

FuncType FuncType::clone(TypeRange inputs, TypeRange results) const {
    assert(results.size() == 1 && "expected exactly one result type");
    return get(llvm::to_vector(inputs), results[0]);
}

//===----------------------------------------------------------------------===//
// StructLikeType
//===----------------------------------------------------------------------===//

namespace mlir::bluefield3::drmt {
bool operator==(const FieldInfo &a, const FieldInfo &b) {
    return a.name == b.name && a.type == b.type;
}
llvm::hash_code hash_value(const FieldInfo &fi) { return llvm::hash_combine(fi.name, fi.type); }
}

static ParseResult parseFields(AsmParser &p, std::string &name,
                               SmallVectorImpl<FieldInfo> &parameters,
                               mlir::DictionaryAttr &annotations) {
    llvm::StringSet<> nameSet;
    mlir::NamedAttrList annList;
    bool hasDuplicateName = false;
    bool parsedName = false;
    auto parseResult =
        p.parseCommaSeparatedList(mlir::AsmParser::Delimiter::LessGreater, [&]() -> ParseResult {
            // First, try to parse name
            if (!parsedName) {
                if (p.parseKeywordOrString(&name) || p.parseOptionalAttrDict(annList))
                    return failure();
                parsedName = true;
                annotations = annList.getDictionary(p.getContext());
                return success();
            }

            // Parse fields
            std::string fieldName;
            Type fieldType;
            mlir::NamedAttrList fieldAnnotations;

            auto fieldLoc = p.getCurrentLocation();
            if (p.parseKeywordOrString(&fieldName) || p.parseColon() || p.parseType(fieldType) ||
                p.parseOptionalAttrDict(fieldAnnotations))
                return failure();

            if (!nameSet.insert(fieldName).second) {
                p.emitError(fieldLoc, "duplicate field name \'" + name + "\'");
                // Continue parsing to print all duplicates, but make sure to error
                // eventually
                hasDuplicateName = true;
            }

            parameters.emplace_back(StringAttr::get(p.getContext(), fieldName), fieldType,
                                    fieldAnnotations.getDictionary(p.getContext()));
            return success();
        });

    if (hasDuplicateName) return failure();
    return parseResult;
}

/// Print out a list of named fields surrounded by <>.
static void printFields(AsmPrinter &p, StringRef name, ArrayRef<FieldInfo> fields,
                        mlir::DictionaryAttr annotations) {
    p << '<';
    p.printString(name);
    if (annotations && !annotations.empty()) {
        p << ' ';
        p.printAttributeWithoutType(annotations);
    }
    if (!fields.empty()) p << ", ";
    llvm::interleaveComma(fields, p, [&](const FieldInfo &field) {
        p.printKeywordOrString(field.name.getValue());
        p << ": " << field.type;
        if (field.annotations && !field.annotations.empty()) {
            p << " ";
            p.printAttributeWithoutType(field.annotations);
        }
    });
    p << ">";
}

//===----------------------------------------------------------------------===//
// StructType
//===----------------------------------------------------------------------===//
Type StructType::parse(AsmParser &p) {
    llvm::SmallVector<FieldInfo, 4> parameters;
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
                                  ArrayRef<FieldInfo> elements,
                                  DictionaryAttr annotations) {
    // Basic verification - ensure no duplicate field names
    llvm::StringSet<> fieldNames;
    for (const auto &field : elements) {
        if (!fieldNames.insert(field.name.getValue()).second) {
            return emitError() << "duplicate field name '" << field.name.getValue() << "'";
        }
    }
    return success();
}

//===----------------------------------------------------------------------===//
// HeaderType
//===----------------------------------------------------------------------===//
Type HeaderType::parse(AsmParser &p) {
    llvm::SmallVector<FieldInfo, 4> parameters;
    std::string name;
    mlir::DictionaryAttr annotations;
    if (parseFields(p, name, parameters, annotations)) return {};
    // Use the standard get() which uses the default builder from the base class
    return get(p.getContext(), name, parameters, annotations);
}

void HeaderType::print(AsmPrinter &p) const {
    printFields(p, getName(), getElements(), getAnnotations());
}

LogicalResult HeaderType::verify(function_ref<InFlightDiagnostic()> emitError,
                                  StringRef name,
                                  ArrayRef<FieldInfo> elements,
                                  DictionaryAttr annotations) {
    // Basic verification - ensure no duplicate field names
    llvm::StringSet<> fieldNames;
    for (const auto &field : elements) {
        if (!fieldNames.insert(field.name.getValue()).second) {
            return emitError() << "duplicate field name '" << field.name.getValue() << "'";
        }
    }
    return success();
}

void vDRMTDialect::registerTypes() {
    addTypes<
#define GET_TYPEDEF_LIST
#include "Dialect/vDRMT/IR/vDRMTTypes.cpp.inc"
        >();
}
