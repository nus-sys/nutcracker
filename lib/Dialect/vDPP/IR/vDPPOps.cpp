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
// AllocaOp
//===----------------------------------------------------------------------===//

llvm::SmallVector<mlir::MemorySlot> vdpp::AllocaOp::getPromotableSlots() {
    // Return the memory slot for this alloca
    auto ptrType = mlir::cast<vdpp::PointerType>(getResult().getType());
    return {MemorySlot{getResult(), ptrType.getElementType()}};
}

mlir::Value vdpp::AllocaOp::getDefaultValue(
    const MemorySlot &slot, OpBuilder &builder) {
    // Return a default value for the slot
    return {};
}

void vdpp::AllocaOp::handleBlockArgument(
    const MemorySlot &slot, BlockArgument argument, OpBuilder &builder) {
    // Handle block argument for memory slot
}

std::optional<mlir::PromotableAllocationOpInterface> 
vdpp::AllocaOp::handlePromotionComplete(
    const MemorySlot &slot, Value defaultValue, OpBuilder &builder) {
    if (defaultValue && defaultValue.use_empty())
        defaultValue.getDefiningOp()->erase();
    this->erase();
    return {};
}

//===----------------------------------------------------------------------===//
// CondBranchOp
//===----------------------------------------------------------------------===//

SuccessorOperands vdpp::CondBranchOp::getSuccessorOperands(unsigned index) {
    assert(index < 2 && "invalid successor index");
    return SuccessorOperands(index == 0 ? getTrueDestOperandsMutable() 
                                        : getFalseDestOperandsMutable());
}

// CompressOp / DecompressOp: $output_len must point to an integer slot so
// the BF/ARM lowering can write the produced byte count back through it.
static mlir::LogicalResult verifyVdppOutputLenPtr(mlir::Operation *op,
                                                  mlir::Value outputLen) {
    auto ptrTy = mlir::dyn_cast<vdpp::PointerType>(outputLen.getType());
    if (!ptrTy)
        return op->emitOpError("output_len must be a vdpp.ptr type");
    if (!ptrTy.getElementType().isIntOrIndex())
        return op->emitOpError(
            "output_len must point to an integer/index slot, got ")
            << ptrTy.getElementType();
    return mlir::success();
}

mlir::LogicalResult vdpp::CompressOp::verify() {
    return verifyVdppOutputLenPtr(*this, getOutputLen());
}

mlir::LogicalResult vdpp::DecompressOp::verify() {
    return verifyVdppOutputLenPtr(*this, getOutputLen());
}

#define GET_OP_CLASSES
#include "Dialect/vDPP/IR/vDPPDialect.cpp.inc"
#include "Dialect/vDPP/IR/vDPPOps.cpp.inc"
#include "Dialect/vDPP/IR/vDPPOpsEnums.cpp.inc"