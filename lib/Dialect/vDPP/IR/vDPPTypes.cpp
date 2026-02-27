//===- vDPPTypes.cpp - Virtual DPP types ---------------*- C++ -*-===//
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
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOpsEnums.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

using namespace mlir;
using namespace mlir::vdpp;

static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params,
                                       mlir::Type &optionalResultType);
static mlir::ParseResult parseFuncType(mlir::AsmParser &p, llvm::SmallVector<mlir::Type> &params);

static void printFuncType(mlir::AsmPrinter &p, mlir::ArrayRef<mlir::Type> params,
                          mlir::Type optionalResultType = {});
                          
#define GET_TYPEDEF_CLASSES
#include "Dialect/vDPP/IR/vDPPTypes.cpp.inc"

void vDPPDialect::printType(mlir::Type type, mlir::DialectAsmPrinter &os) const {
    return;
}

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

// Return the actual return type or an explicit !vdpp.void if the function does
// not return anything
mlir::Type FuncType::getReturnType() const {
    if (isVoid()) return vdpp::VoidType::get(getContext());
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

void vDPPDialect::registerTypes() {
    addTypes<
#define GET_TYPEDEF_LIST
#include "Dialect/vDPP/IR/vDPPTypes.cpp.inc"
        >();
}
