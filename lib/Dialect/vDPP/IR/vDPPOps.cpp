//===- vDPPOps.cpp - Virtual DPP dialect ops ---------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <string>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/LogicalResult.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Interfaces/FunctionImplementation.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/InliningUtils.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"

#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

using namespace mlir;
using namespace mlir::LLVM;
using namespace mlir::vdpp;

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

void vdpp::ConstantOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
    // Since there's no name attribute, just use a generic name
    setNameFn(getResult(), "cst");
}

LogicalResult vdpp::ConstantOp::verify() {
    return success();
}

OpFoldResult vdpp::ConstantOp::fold(FoldAdaptor adaptor) { return getValue(); }

//===----------------------------------------------------------------------===//
// BinaryOp
//===----------------------------------------------------------------------===//

void vdpp::BinOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
    setNameFn(getResult(), stringifyEnum(getKind()));
}

#define GET_OP_CLASSES
#include "Dialect/vDPP/IR/vDPPDialect.cpp.inc"
#include "Dialect/vDPP/IR/vDPPOps.cpp.inc"
#include "Dialect/vDPP/IR/vDPPOpsEnums.cpp.inc"