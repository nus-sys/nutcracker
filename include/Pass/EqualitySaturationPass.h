#ifndef EQUALITYSATURATIONPASS_H
#define EQUALITYSATURATIONPASS_H

#include <set>
#include <map>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

#include "llvm/Support/FileSystem.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "Dialect/P4HIR/P4HIR_Ops.h"

#include "Pass/Egglog.h"
#include "Pass/Utils.h"

struct EqualitySaturationPass : public mlir::PassWrapper<EqualitySaturationPass, mlir::OperationPass<mlir::ModuleOp>> {
    std::string mlirFilePath;
    std::string eggFilePath;

    std::string egglogExtractedFuncFilename = "egglog-func-extract.txt";
    std::string egglogExtractedFilename = "egglog-extract.txt";
    std::string egglogLogFilename = "egglog-log.txt";
    
    EgglogCustomDefs customFunctions;

    std::map<std::string, EgglogOpDef> supportedOps;
    std::set<std::string> supportedDialects;

    double mlirToEgglogTime = 0.0;
    double egglogExecTime = 0.0;
    double egglogToMlirTime = 0.0;

    EqualitySaturationPass(const std::string&, const std::string&, const EgglogCustomDefs&);

    mlir::StringRef getArgument() const override { return "eq-sat"; }
    mlir::StringRef getDescription() const override { return "Performs equality saturation on each block in the given file. The language definition is egglog."; }
    
    void init();
    void runOnOperation() override;
    // void convertRootOpToBf3drmt(P4::P4MLIR::P4HIR::ControlOp oldControlOp);
    void runOnBlock(mlir::Block& block, const std::string& blockName);

    void runOnFunction(mlir::func::FuncOp& funcOp);

// void runEgglog(const std::vector<EggifiedOp*>& block, const std::string& blockName);
    void runEgglog(const std::vector<EggifiedOp*>& block, const std::vector<EggifiedOp*>& rootBlock, const std::string& blockName);
};

std::unique_ptr<mlir::Pass> createEqualitySaturationPass(const std::string&, const std::string&, const EgglogCustomDefs&);
#endif  // EQUALITYSATURATIONPASS_H
