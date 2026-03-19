#ifndef EGGIFY_PASS_H
#define EGGIFY_PASS_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "Dialect/P4HIR/P4HIR_Ops.h"
#include "mlir/Pass/Pass.h"

// TODO fix this path: we changed the output of an operation in egglog so make sure it is consistent

// struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<mlir::func::FuncOp>> {
// struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<mlir::LLVM::LLVMFuncOp>> {
// struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<P4::P4MLIR::P4HIR::FuncOp>> {
// struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<P4::P4MLIR::P4HIR::ControlOp>> {
// struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<P4::P4MLIR::P4HIR::TableKeyOp>> {
struct EggifyPass : public mlir::PassWrapper<EggifyPass, mlir::OperationPass<mlir::ModuleOp>> {
    mlir::StringRef getArgument() const override { return "eggify"; }
    mlir::StringRef getDescription() const override { return "Converts MLIR operations to Egglog Op variants."; }

    void runOnOperation() override {
        // mlir::func::FuncOp rootOp = getOperation();
        // mlir::LLVM::LLVMFuncOp rootOp = getOperation();
        // P4::P4MLIR::P4HIR::FuncOp rootOp = getOperation();
        // P4::P4MLIR::P4HIR::ControlOp rootOp = getOperation();
        // P4::P4MLIR::P4HIR::TableKeyOp rootOp = getOperation();
        mlir::ModuleOp rootOp = getOperation();

        // Get the name of the function
        // llvm::StringRef rootOpName = rootOp.getName();
        llvm::StringRef rootOpName = "test";

        llvm::outs() << "--------------------------------\n";
        llvm::outs() << "Function: " << rootOpName << "\n";

        // Get all operations in the function
        rootOp.walk([](mlir::Operation* op) {
            mlir::OperationName opName = op->getName();
            llvm::StringRef opNameStr = opName.getStringRef();
            size_t numOperands = op->getNumOperands();
            size_t numAttributes = opName.getAttributeNames().size();

            // Print the operation name and the number of operands, results, and attributes
            // (function <opName> (i64 [Op]*numOperands [AttrPair]*numAttributes Type) Op)
            llvm::outs() << "(function " << opNameStr << " (i64";  // (function <opName> i64)
            for (size_t i = 0; i < numOperands; i++) {
                llvm::outs() << " Op";
            }
            for (size_t i = 0; i < numAttributes; i++) {
                llvm::outs() << " AttrPair";
            }
            llvm::outs() << " Type) Op)\n";
        });

        llvm::outs() << "--------------------------------\n";
    }
};

#endif  // EGGIFY_PASS_H