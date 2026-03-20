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
#include <cstdlib>

#include "llvm/ADT/Hashing.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

#include "Dialect/P4HIR/P4HIR_Ops.h"
#include "Dialect/P4HIR/P4HIR_Types.h"

#include "Dialect/Bf3/Drmt/IR/Bf3DrmtOps.h"
#include "Dialect/Bf3/Drmt/IR/Bf3DrmtTypes.h"

#include "Pass/EqualitySaturationPass.h"
#include "Pass/Egglog.h"

EqualitySaturationPass::EqualitySaturationPass(const std::string& mlirFile, const std::string& eggFile, const EgglogCustomDefs& funcs)
    : mlirFilePath(mlirFile), eggFilePath(eggFile), customFunctions(funcs) {}

void EqualitySaturationPass::runEgglog(const std::vector<EggifiedOp*>& block, const std::vector<EggifiedOp*>& rootBlock, const std::string& blockName) {
    std::ifstream eggFile(eggFilePath);
    std::vector<std::string> egglogLines;

    // Read the egglog file
    std::string opsTarget = ";; OPS HERE ;;";
    std::string extractsTarget = ";; EXTRACTS HERE ;;";

    bool insertedOps = false;
    bool insertedExtracts = false;

    auto start = std::chrono::high_resolution_clock::now();

    std::string line;
    while (std::getline(eggFile, line)) {
        egglogLines.push_back(line);

        if (!insertedOps && line == opsTarget) {
            egglogLines.push_back("; " + blockName);
            for (const EggifiedOp* op: block) {  // Insert the operations
                egglogLines.push_back(op->egglogLet());
            }

            insertedOps = true;
        } else if (!insertedExtracts && line == extractsTarget) {
            // for (const EggifiedOp* op: rootBlock) {  // Extract the results of the egglog run
            for (const EggifiedOp* op: block) {  // Extract the results of the egglog run
                if (op->shouldBeExtracted()) {
                    egglogLines.push_back("(extract " + op->getPrintId() + ")");
                }
            }

            insertedExtracts = true;
        }
    }
    eggFile.close();

    // Write the extracted egglog to the output directory (next to the log files)
    std::string opsEggFilePath = mlirFilePath.substr(0, mlirFilePath.find(".mlir")) + ".ops.egg";
    std::ofstream eggFileOut(opsEggFilePath);
    for (const std::string& line: egglogLines) {
        eggFileOut << line << "\n";
    }
    eggFileOut.close();
    
    auto end = std::chrono::high_resolution_clock::now();
    mlirToEgglogTime += std::chrono::duration<double>(end - start).count();

    // Run egglog and extract the results
    // Check for EGGLOG environment variable for the path
    const char* egglogEnv = std::getenv("EGGLOG");
    std::string egglogPath = egglogEnv ? std::string(egglogEnv) + "/egglog" : "egglog";
    std::string egglogCmd = egglogPath + " " + opsEggFilePath + " > " + egglogExtractedFilename + " 2> " + egglogLogFilename;

    llvm::outs() << "\nRunning egglog: " << egglogCmd << "\n"
                 << "\n";

    start = std::chrono::high_resolution_clock::now();
    std::system(egglogCmd.c_str());
    end = std::chrono::high_resolution_clock::now();

    egglogExecTime += std::chrono::duration<double>(end - start).count();

    // dump output
    printFileContents(egglogLogFilename);
    printFileContents(egglogExtractedFilename);

    llvm::outs() << "\n\nDone running egglog\n"
                 << "\n";
}

void EqualitySaturationPass::runOnBlock(mlir::Block& block, const std::string& blockName) {
    auto start = std::chrono::high_resolution_clock::now();

    // Eggify the block
    mlir::MLIRContext& context = getContext();
    Egglog egglog(context, customFunctions, supportedOps);

    // register all block arguments
    for (mlir::Value value: block.getArguments()) {
        EggifiedOp* eggifiedValue = egglog.eggifyValue(value);
        // eggifiedValue->print(llvm::outs());
    }

    for (mlir::Operation& op: block.getOperations()) {
        EggifiedOp* eggifiedOp = egglog.eggifyOperation(&op);
        // eggifiedOp->print(llvm::outs());
        egglog.rootEggifiedBlock.push_back(eggifiedOp); 
    }

    auto end = std::chrono::high_resolution_clock::now();
    mlirToEgglogTime += std::chrono::duration<double>(end - start).count();

    // dump all ops
    for (const EggifiedOp* eggOp: egglog.eggifiedBlock) {
        eggOp->print(llvm::outs());
    }

    // runEgglog(egglog.eggifiedBlock, blockName);
    runEgglog(egglog.eggifiedBlock, egglog.rootEggifiedBlock, blockName);

#if 0
    // Read the extracted results
    std::vector<std::string> extractedOps;
    {
        std::ifstream extractedFile(egglogExtractedFilename);
        std::string line;
        while (std::getline(extractedFile, line)) {
            extractedOps.push_back(line);
        }
        extractedFile.close();
    }
    // Generate comparison egg file
    std::string comparisonFilename = mlirFilePath.substr(0, mlirFilePath.find(".mlir")) + "-comparison.egg";
    std::error_code EC;
    llvm::raw_fd_ostream outFile(comparisonFilename, EC);
    if (EC) {
        llvm::errs() << "Could not open file: " << EC.message() << "\n";
        return;
    }

    // Write header with metadata
    outFile << ";; Generated by DialEgg on 2025-08-12 13:35:12\n";  // Updated timestamp
    outFile << ";; User: yihan1998\n\n";

    // Write base includes and type definitions
    outFile << "(include \"egglog_rules/base.egg\")\n\n"
           << ";; Hardware mapping flags\n"
           << "(datatype HwMapFlags\n"
           << "    (cpu)\n"
           << "    (bf3drmt))\n\n"
           << ";; Stage functions\n"
           << "(function stage (HwMapFlags OpVec) Block)\n\n";

    // Write dialect definitions
    outFile << ";;;; p4hir dialect ;;;;\n"
           << "(function p4hir_bits (i64) Type :cost 100)\n"
           << "(function p4hir_int (i64 Type) Attr :cost 100)\n"
           << "(function p4hir_const (AttrPair Type) Op :cost 100)\n"
           << "(function p4hir_binop (Op Op AttrPair Type) Op :cost 1000)\n"
           << "(function p4hir_shr (Op Op Type) Op :cost 100)\n"
           << "(function p4hir_variable (AttrPair AttrPair Type) Op :cost 100)\n"
           << "(function p4hir_assign (Op Op) Op :cost 100)\n"
           << "(function p4hir_ref (Type) Type :cost 100)\n"
           << "(function p4hir_read (Op Type) Op :cost 100)\n\n"
           << ";;;; drmt dialect ;;;;\n"
           << "(function drmt_bits (i64) Type :cost 90)\n"
           << "(function drmt_int (i64 Type) Attr :cost 90)\n"
           << "(function drmt_constant (AttrPair Type) Op :cost 90)\n"
           << "(function drmt_addi (Op Op Type) Op :cost 90)\n"
           << "(function drmt_subi (Op Op Type) Op :cost 90)\n"
           << "(function drmt_shli (Op Op Type) Op :cost 90)\n"
           << "(function drmt_shrui (Op Op Type) Op :cost 90)\n"
           << "(function drmt_variable (AttrPair AttrPair Type) Op :cost 90)\n"
           << "(function drmt_assign (Op Op) Op :cost 90)\n"
           << "(function drmt_ref (Type) Type :cost 100)\n"
           << "(function drmt_read (Op Type) Op :cost 90)\n\n"
           << ";;;; bf3drmt dialect ;;;;\n"
           << "(function bf3drmt_bits (i64) Type :cost 1)\n"
           << "(function bf3drmt_int (i64 Type) Attr :cost 1)\n"
           << "(function bf3drmt_constant (AttrPair Type) Op :cost 1)\n"
           << "(function bf3drmt_addi (Op Op Type) Op :cost 1)\n"
           << "(function bf3drmt_subi (Op Op Type) Op :cost 1)\n"
           << "(function bf3drmt_shli (Op Op Type) Op :cost 1)\n"
           << "(function bf3drmt_shrui (Op Op Type) Op :cost 1)\n"
           << "(function bf3drmt_assign (Op Op) Op :cost 1)\n\n";

    // Replace dots with underscores in blockName
    std::string sanitizedBlockName = blockName;
    std::replace(sanitizedBlockName.begin(), sanitizedBlockName.end(), '.', '_');

    // Write original P4HIR operations
    outFile << ";; Define original P4HIR operations\n";
    for (size_t i = 0; i < egglog.eggifiedBlock.size(); i++) {
        outFile << "(let op" << i << " ";
        std::string str;
        llvm::raw_string_ostream ss(str);
        egglog.eggifiedBlock[i]->print(ss);
        std::string output = ss.str();
        size_t start = output.find('(');
        size_t end = output.find(" FROM");
        if (start != std::string::npos && end != std::string::npos) {
            outFile << output.substr(start, end - start) << ")\n";
        } else {
            start = output.find("Value");
            if (start != std::string::npos) {
                end = output.find(" FROM");
                outFile << "(" << output.substr(start, end - start) << "))\n";
            }
        }
    }
    outFile << "\n(let " << sanitizedBlockName << "_p4hir\n    (vec-of";
    for (size_t i = 0; i < egglog.eggifiedBlock.size(); i++) {
        outFile << " op" << i;
    }
    outFile << "))\n\n";

    // Write transformed BF3DRMT operations
    outFile << ";; Define lowered BF3DRMT operations\n";
    size_t opCount = egglog.eggifiedBlock.size();
    for (size_t i = 0; i < extractedOps.size(); i++) {
        outFile << "(let op" << (i + opCount) << " " << extractedOps[i] << ")\n";
    }
    outFile << "\n(let " << sanitizedBlockName << "_bf3drmt\n    (vec-of";
    for (size_t i = 0; i < extractedOps.size(); i++) {
        outFile << " op" << (i + opCount);
    }
    outFile << "))\n\n";

    // Write stages and rules
    outFile << ";; Create stages\n"
           << "(let s1 (stage (cpu) " << sanitizedBlockName << "_p4hir))\n"
           << "(let s2 (stage (bf3drmt) " << sanitizedBlockName << "_bf3drmt))\n\n"
           << "(ruleset rules)\n\n"
           << ";; Transform stages while considering dependencies\n"
           << "(rewrite\n"
           << "    (stage (cpu) " << sanitizedBlockName << "_p4hir)\n"
           << "    (stage (bf3drmt) " << sanitizedBlockName << "_bf3drmt)\n"
           << "    :ruleset rules)\n\n"
           << "(run-schedule (saturate rules))\n"
           << "(extract s1)\n";

    outFile.close();
#endif
#if 0
    start = std::chrono::high_resolution_clock::now();

    std::ifstream file(egglogExtractedFilename);

    // Parse the extracted egglog file and replace the MLIR operations
    for (const EggifiedOp* eggOp: egglog.eggifiedBlock) {
        if (!eggOp->shouldBeExtracted()) {
            continue;
        }

        std::string line;
        std::getline(file, line);

        mlir::Operation* prevOp = eggOp->mlirOp;
        mlir::OpBuilder builder(prevOp);
        mlir::Operation* newOp = egglog.parseOperation(line, builder);

        if(newOp == nullptr) { // If the operation is an opaque value, replace it with the value
            mlir::Value value = egglog.parseValue(line);
            prevOp->getResult(0).replaceAllUsesWith(value);
            prevOp->erase();
        } else if (newOp != prevOp) { // Check if the whole operation is different, if so, replace it
            // llvm::outs() << "REPLACING: (" << *prevOp << ") WITH (" << *newOp << ")\n\n";
            prevOp->replaceAllUsesWith(newOp);
            prevOp->erase();
        }

        // llvm::outs() << "\n";
    }

    end = std::chrono::high_resolution_clock::now();

    egglogToMlirTime += std::chrono::duration<double>(end - start).count();

    // llvm::outs() << "\n\nDone, printing ops: \n";
    // dump parsed ops cache
    for (const auto& [opStr, op]: egglog.parsedOps) {
        llvm::outs() << opStr << " : " << *op << "\n";
    }
    file.close();
#endif
    llvm::outs() << "\n";
}

void EqualitySaturationPass::runOnFunction(mlir::func::FuncOp& funcOp) {
    llvm::outs() << "Running on function: " << funcOp.getName() << "\n";
    auto start = std::chrono::high_resolution_clock::now();

    // Eggify the function
    mlir::MLIRContext& context = getContext();
    Egglog egglog(context, customFunctions, supportedOps);

    // Map to track which block each operation belongs to
    std::map<mlir::Block*, std::vector<EggifiedOp*>> blockToOps;

    // Register function arguments
    for (mlir::Value value: funcOp.getArguments()) {
        EggifiedOp* eggifiedValue = egglog.eggifyValue(value);
        eggifiedValue->print(llvm::outs());
    }

    // Register operations from all blocks while keeping track of block ownership
    for (mlir::Block& block : funcOp.getRegion().getBlocks()) {
        blockToOps[&block] = std::vector<EggifiedOp*>();
        
        for (mlir::Operation& op: block.getOperations()) {
            EggifiedOp* eggifiedOp = egglog.eggifyOperation(&op);
            eggifiedOp->print(llvm::outs());
            blockToOps[&block].push_back(eggifiedOp);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    mlirToEgglogTime += std::chrono::duration<double>(end - start).count();

    // dump all ops
    for (const EggifiedOp* eggOp: egglog.eggifiedBlock) {
        eggOp->print(llvm::outs());
    }

    // DANGER: This is a temporary solution to run Egglog on the function.
    runEgglog(egglog.eggifiedBlock, egglog.rootEggifiedBlock, funcOp.getName().str());

    // Read the extracted results
    std::vector<std::string> extractedOps;
    {
        std::ifstream extractedFile(egglogExtractedFilename);
        std::string line;
        while (std::getline(extractedFile, line)) {
            extractedOps.push_back(line);
        }
        extractedFile.close();
    }

    start = std::chrono::high_resolution_clock::now();

    std::ifstream file(egglogExtractedFilename);

    // Open a new file for block structure output
    std::ofstream egglogExtractedFuncFile(egglogExtractedFuncFilename);
    
    // Process and display operations block by block
    int blockIndex = 0;
    for (mlir::Block& block : funcOp.getRegion().getBlocks()) {
        egglogExtractedFuncFile << "\n^bb" << blockIndex++ << ":";
        
        // Show block arguments if any
        if (block.getNumArguments() > 0) {
            egglogExtractedFuncFile << "(";
            for (mlir::BlockArgument arg : block.getArguments()) {
                std::string typeStr;
                llvm::raw_string_ostream os(typeStr);
                arg.getType().print(os);
                egglogExtractedFuncFile << "%arg" << arg.getArgNumber() << ": " 
                                 << typeStr << ", ";
            }
            egglogExtractedFuncFile << ")";
        }
        
        // Show block predecessors
        if (!block.hasNoPredecessors()) {
            egglogExtractedFuncFile << "  // pred: ";
            int predIndex = 0;
            for (mlir::Block* pred : block.getPredecessors()) {
                // Find predecessor block index by counting blocks
                int predBlockIndex = 0;
                for (mlir::Block& b : funcOp.getRegion().getBlocks()) {
                    if (&b == pred) break;
                    predBlockIndex++;
                }
                egglogExtractedFuncFile << "^bb" << predBlockIndex << " ";
            }
        }
        egglogExtractedFuncFile << "\n";

        // Process operations in this block
        for (const EggifiedOp* eggOp : blockToOps[&block]) {
            if (!eggOp->shouldBeExtracted()) {
                continue;
            }

            std::string line;
            std::getline(file, line);
            egglogExtractedFuncFile << "    " << line << "\n";  // Indent operations

            mlir::Operation* prevOp = eggOp->mlirOp;
            mlir::OpBuilder builder(prevOp);
            mlir::Operation* newOp = egglog.parseOperation(line, builder);

            if(newOp == nullptr) {
                mlir::Value value = egglog.parseValue(line);
                prevOp->getResult(0).replaceAllUsesWith(value);
                prevOp->erase();
            } else if (newOp != prevOp) {
                prevOp->replaceAllUsesWith(newOp);
                prevOp->erase();
            }
        }
    }

    end = std::chrono::high_resolution_clock::now();
    egglogToMlirTime += std::chrono::duration<double>(end - start).count();

    file.close();
    egglogExtractedFuncFile.close();
    llvm::outs() << "\n";
}

void EqualitySaturationPass::init() {
    // Make sure both files exist
    if (!llvm::sys::fs::exists(mlirFilePath)) {
        llvm::errs() << "MLIR file does not exist: " << mlirFilePath << "\n";
        exit(1);
    }
    if (!llvm::sys::fs::exists(eggFilePath)) {
        llvm::errs() << "Egg file does not exist: " << eggFilePath << "\n";
        exit(1);
    }

    // mlirFilePath without extension
    std::string name = mlirFilePath.substr(0, mlirFilePath.find(".mlir"));
    egglogExtractedFuncFilename = name + "-egglog-func-extract.log";
    egglogExtractedFilename = name + "-egglog-extract.log";
    egglogLogFilename = name + "-egglog.log";
    
    std::ifstream opFile(eggFilePath);
    std::string line;

    while (std::getline(opFile, line)) {
        // llvm::outs() << "Parsing line: " << line << "\n";
        if (EgglogOpDef::isOpFunction(line)) {
            EgglogOpDef parsedOp = EgglogOpDef::parseOpFunction(line);

            supportedOps.emplace(parsedOp.dialect + "." + parsedOp.name + (parsedOp.version.empty() ? "" : "." + parsedOp.version), parsedOp);
            supportedOps.emplace(parsedOp.dialect + "_" + parsedOp.name + (parsedOp.version.empty() ? "" : "_" + parsedOp.version), parsedOp);
            supportedDialects.insert(parsedOp.dialect);
        }
    }

    opFile.close();

    // dump
    // llvm::outs() << "Supported ops: ";
    // for (const auto& [op, _]: supportedOps) {
    //     llvm::outs() << op << ", ";
    // }

    // llvm::outs() << "\nSupported dialects: ";
    // for (const std::string& dialect: supportedDialects) {
    //     llvm::outs() << dialect << ", ";
    // }

    // llvm::outs() << "\n\n";
}

// void EqualitySaturationPass::convertRootOpToBf3drmt(P4::P4MLIR::P4HIR::ControlOp oldControlOp) {
//     auto parentOp = oldControlOp->getParentOp();
//     if (!parentOp) {
//         llvm::errs() << "Error: ControlOp has no parent operation\n";
//         return;
//     }
    
//     mlir::OpBuilder builder(parentOp);
//     builder.setInsertionPoint(oldControlOp);
//     mlir::Location loc = oldControlOp.getLoc();
    
//     // Get the attributes from the old control op
//     llvm::StringRef symName = oldControlOp.getName();
    
//     // Get arg_attrs if it exists
//     llvm::ArrayRef<mlir::DictionaryAttr> argAttrs;
//     if (auto argAttrsAttr = oldControlOp->getAttrOfType<mlir::ArrayAttr>("arg_attrs")) {
//         // Convert ArrayAttr to ArrayRef<DictionaryAttr>
//         llvm::SmallVector<mlir::DictionaryAttr> attrs;
//         for (auto attr : argAttrsAttr) {
//             if (auto dictAttr = attr.dyn_cast<mlir::DictionaryAttr>()) {
//                 attrs.push_back(dictAttr);
//             }
//         }
//         argAttrs = attrs;
//     }
    
//     // Get annotations if they exist
//     mlir::DictionaryAttr annotations;
//     if (auto annotationsAttr = oldControlOp->getAttrOfType<mlir::DictionaryAttr>("annotations")) {
//         annotations = annotationsAttr;
//     }
    
//     // Create the new bf3drmt.control operation
//     auto newControlOp = builder.create<mlir::edamlir::bf3drmt::ControlOp>(
//         loc,
//         symName,
//         argAttrs,
//         annotations
//     );

//     // Move the region from old to new operation
//     newControlOp.getBody().takeBody(oldControlOp.getRegion());

//     // Copy any other attributes you need
//     if (auto symVisibility = oldControlOp->getAttr("sym_visibility")) {
//         newControlOp->setAttr("sym_visibility", symVisibility);
//     }

//     newControlOp->dump();

//     // Replace and erase the old operation
//     oldControlOp->replaceAllUsesWith(newControlOp);
//     oldControlOp->erase();
// }
#if 0
void EqualitySaturationPass::runOnOperation() {
    init();

    P4::P4MLIR::P4HIR::ControlOp rootOp = getOperation();
    auto parentOp = rootOp->getParentOp();
    
    // Check if we have a parent to work with
    if (!parentOp) {
        llvm::errs() << "Error: ControlOp has no parent operation, cannot rewrite root\n";
        signalPassFailure();
        return;
    }
    
    llvm::StringRef rootOpName = rootOp.getName();

    llvm::outs() << "Parent operation: " << parentOp->getName().getStringRef() << "\n";
    llvm::outs() << "Running on function: " << rootOpName << "\n";
    llvm::outs() << "-----------------------------------------\n";

    // Perform equality saturation on all operations of a block.
    for (mlir::Block& block: rootOp.getRegion().getBlocks()) {
        std::string parentOpName = block.getParentOp()->getName().getStringRef().str();
        std::string blockName = rootOpName.str() + "_" + parentOpName;
        runOnBlock(block, blockName);

        llvm::outs() << "After running on block: " << blockName << "\n";

        llvm::outs() << "Block arguments: ";
        llvm::SmallVector<mlir::Type> argTypes;
        for (mlir::BlockArgument arg : block.getArguments()) {
            arg.getType().print(llvm::outs());
            llvm::outs() << "\n";
        }
        llvm::outs() << "\n";

        // Temporary dead code elimination
        bool clean = false;
        while (!clean) {
            clean = true;
            
            block.walk([&](mlir::Operation* op) {
                if (mlir::isOpTriviallyDead(op)) {
                    clean = false;
                    op->erase();
                }
            });
        }
    }

    llvm::outs() << "-----------------------------------------\n";
    llvm::outs() << "Done running on function: " << rootOpName << "\n";
    llvm::outs() << "mlirToEgglogTime = " << mlirToEgglogTime << "s\n";
    llvm::outs() << "egglogExecTime = " << egglogExecTime << "s\n";
    llvm::outs() << "egglogToMlirTime = " << egglogToMlirTime << "s\n";
    llvm::outs() << "-----------------------------------------\n";

    parentOp->dump();
}
#endif

void EqualitySaturationPass::runOnOperation() {
    init();

    mlir::ModuleOp moduleOp = getOperation();

#if 0
    llvm::SmallVector<P4::P4MLIR::P4HIR::ControlOp, 4> opsToRewrite;
    for (auto controlOp : moduleOp.getOps<P4::P4MLIR::P4HIR::ControlOp>()) {
        opsToRewrite.push_back(controlOp);
    }

    for (auto controlOp : opsToRewrite) {
        // Extract the symbol name from the old operation
        llvm::StringRef symName;
        if (auto symNameAttr = controlOp->getAttrOfType<mlir::StringAttr>("sym_name")) {
            symName = symNameAttr.getValue();
        } else {
            // If no sym_name attribute, create a default one or skip
            symName = "default_control";
        }

        // Extract argAttrs if they exist
        llvm::SmallVector<mlir::DictionaryAttr, 4> argAttrs;
        if (auto argAttrsArrayAttr = controlOp->getAttrOfType<mlir::ArrayAttr>("argAttrs")) {
            for (auto attr : argAttrsArrayAttr) {
                if (auto dictAttr = attr.dyn_cast<mlir::DictionaryAttr>()) {
                    argAttrs.push_back(dictAttr);
                }
            }
        }

        // Extract annotations if they exist
        mlir::DictionaryAttr annotations;
        if (auto annotationsAttr = controlOp->getAttrOfType<mlir::DictionaryAttr>("annotations")) {
            annotations = annotationsAttr;
        }

        // Create new bf3drmt::ControlOp with correct parameters
        mlir::OpBuilder builder(controlOp);
        auto newOp = builder.create<mlir::edamlir::bf3drmt::ControlOp>(
            controlOp.getLoc(),
            symName,
            argAttrs,
            annotations
        );

        // Transfer region body (moves, not clones)
        if (controlOp->getNumRegions() > 0 && newOp->getNumRegions() > 0) {
            newOp->getRegion(0).takeBody(controlOp->getRegion(0));
        }

        // Copy remaining attributes (excluding the ones already handled)
        mlir::NamedAttrList newAttrs;
        for (auto namedAttr : controlOp->getAttrs()) {
            llvm::StringRef attrName = namedAttr.getName().getValue();
            // Skip attributes that are already handled in the constructor
            if (attrName != "sym_name" && attrName != "argAttrs" && attrName != "annotations") {
                newAttrs.push_back(namedAttr);
            }
        }
        
        // Set the remaining attributes on the new operation
        for (auto namedAttr : newAttrs) {
            newOp->setAttr(namedAttr.getName(), namedAttr.getValue());
        }

        // Replace uses of the old op's results with the new op's results
        for (unsigned i = 0; i < controlOp->getNumResults() && i < newOp->getNumResults(); ++i) {
            controlOp->getResult(i).replaceAllUsesWith(newOp->getResult(i));
        }

        // Erase the old op
        controlOp.erase();
    }
#endif

#define TEST_P4
#ifdef TEST_P4
    for (auto controlOp : moduleOp.getOps<P4::P4MLIR::P4HIR::ControlOp>()) {
        // llvm::outs() << "Running on ControlOp: " << controlOp.getName() << "\n";
        // controlOp.dump();
        llvm::StringRef rootOpName = controlOp.getName();

        for (mlir::Block& block: controlOp.getRegion().getBlocks()) {
            std::string parentOpName = block.getParentOp()->getName().getStringRef().str();
            std::string blockName = rootOpName.str() + "_" + parentOpName;
            runOnBlock(block, blockName);

            // llvm::outs() << "After running on block: " << blockName << "\n";

            // llvm::outs() << "Block arguments: ";
            // llvm::SmallVector<mlir::Type> argTypes;
            // for (mlir::BlockArgument arg : block.getArguments()) {
            //     arg.getType().print(llvm::outs());
            //     llvm::outs() << "\n";
            // }
            // llvm::outs() << "\n";

            // Temporary dead code elimination
            bool clean = false;
            while (!clean) {
                clean = true;
                
                block.walk([&](mlir::Operation* op) {
                    if (mlir::isOpTriviallyDead(op)) {
                        clean = false;
                        op->erase();
                    }
                });
            }
        }

        llvm::outs() << "-----------------------------------------\n";
        llvm::outs() << "Done running on function: " << rootOpName << "\n";
        llvm::outs() << "mlirToEgglogTime = " << mlirToEgglogTime << "s\n";
        llvm::outs() << "egglogExecTime = " << egglogExecTime << "s\n";
        llvm::outs() << "egglogToMlirTime = " << egglogToMlirTime << "s\n";
        llvm::outs() << "-----------------------------------------\n";
    
    }
#else 
    for (auto funcOp : moduleOp.getOps<mlir::LLVM::LLVMFuncOp>()) {
        llvm::outs() << "Running on funcOp: " << funcOp.getName() << "\n";
        funcOp.dump();
        llvm::StringRef rootOpName = funcOp.getName();

        for (mlir::Block& block: funcOp.getRegion().getBlocks()) {
            std::string parentOpName = block.getParentOp()->getName().getStringRef().str();
            std::string blockName = rootOpName.str() + "_" + parentOpName;
            runOnBlock(block, blockName);

            llvm::outs() << "After running on block: " << blockName << "\n";

            llvm::outs() << "Block arguments: ";
            llvm::SmallVector<mlir::Type> argTypes;
            for (mlir::BlockArgument arg : block.getArguments()) {
                arg.getType().print(llvm::outs());
                llvm::outs() << "\n";
            }
            llvm::outs() << "\n";

            // Temporary dead code elimination
            bool clean = false;
            while (!clean) {
                clean = true;
                
                block.walk([&](mlir::Operation* op) {
                    if (mlir::isOpTriviallyDead(op)) {
                        clean = false;
                        op->erase();
                    }
                });
            }
        }

        llvm::outs() << "-----------------------------------------\n";
        llvm::outs() << "Done running on function: " << rootOpName << "\n";
        llvm::outs() << "mlirToEgglogTime = " << mlirToEgglogTime << "s\n";
        llvm::outs() << "egglogExecTime = " << egglogExecTime << "s\n";
        llvm::outs() << "egglogToMlirTime = " << egglogToMlirTime << "s\n";
        llvm::outs() << "-----------------------------------------\n";
    
    }
#endif
#if 0
    // Create a duplicate of the original ControlOp to work on
    mlir::IRRewriter rewriter(rootOp->getContext());
    rewriter.setInsertionPoint(rootOp);
    
    // Clone the entire ControlOp operation
    mlir::IRMapping mapping;
    auto duplicatedControlOp = mlir::cast<P4::P4MLIR::P4HIR::ControlOp>(
        rewriter.clone(*rootOp.getOperation(), mapping)
    );

    llvm::outs() << "Created duplicate ControlOp for processing\n";

    // Perform equality saturation on all operations of the duplicated block
    for (mlir::Block& block: duplicatedControlOp.getRegion().getBlocks()) {
        std::string parentOpName = block.getParentOp()->getName().getStringRef().str();
        std::string blockName = rootOpName.str() + "_" + parentOpName;
        runOnBlock(block, blockName);

        llvm::outs() << "After running on block: " << blockName << "\n";

        llvm::outs() << "Block arguments: ";
        llvm::SmallVector<mlir::Type> argTypes;
        for (mlir::BlockArgument arg : block.getArguments()) {
            arg.getType().print(llvm::outs());
            llvm::outs() << "\n";
        }
        llvm::outs() << "\n";

        // Temporary dead code elimination
        bool clean = false;
        while (!clean) {
            clean = true;
            
            block.walk([&](mlir::Operation* op) {
                if (mlir::isOpTriviallyDead(op)) {
                    clean = false;
                    op->erase();
                }
            });
        }
    }

    llvm::outs() << "-----------------------------------------\n";
    llvm::outs() << "Done running on function: " << rootOpName << "\n";
    llvm::outs() << "mlirToEgglogTime = " << mlirToEgglogTime << "s\n";
    llvm::outs() << "egglogExecTime = " << egglogExecTime << "s\n";
    llvm::outs() << "egglogToMlirTime = " << egglogToMlirTime << "s\n";
    llvm::outs() << "-----------------------------------------\n";

    // Now create the bf3drmt ControlOp from the optimized duplicate
    mlir::Location loc = rootOp.getLoc();

    // Get the attributes from the original control op
    llvm::StringRef symName = rootOp.getName();

    // Get arg_attrs if it exists
    llvm::ArrayRef<mlir::DictionaryAttr> argAttrs;
    llvm::SmallVector<mlir::DictionaryAttr> argAttrsStorage;
    if (auto argAttrsAttr = rootOp->getAttrOfType<mlir::ArrayAttr>("arg_attrs")) {
        for (auto attr : argAttrsAttr) {
            if (auto dictAttr = attr.dyn_cast<mlir::DictionaryAttr>()) {
                argAttrsStorage.push_back(dictAttr);
            }
        }
        argAttrs = argAttrsStorage;
    }

    // Get annotations if they exist
    mlir::DictionaryAttr annotations;
    if (auto annotationsAttr = rootOp->getAttrOfType<mlir::DictionaryAttr>("annotations")) {
        annotations = annotationsAttr;
    }

    // Create the new bf3drmt.control operation
    auto newControlOp = rewriter.create<mlir::edamlir::bf3drmt::ControlOp>(
        loc,
        symName,
        argAttrs,
        annotations
    );

    // Move the optimized region from the duplicate to the new bf3drmt ControlOp
    newControlOp.getBody().takeBody(duplicatedControlOp.getRegion());

    // Copy any other attributes from the original ControlOp
    if (auto symVisibility = rootOp->getAttr("sym_visibility")) {
        newControlOp->setAttr("sym_visibility", symVisibility);
    }

    llvm::outs() << "\nNew bf3drmt ControlOp created:\n";
    newControlOp->dump();

    // Clean up the duplicate (it's now empty after takeBody)
    duplicatedControlOp->erase();

    // Replace the original operation with the new one
    rewriter.replaceOp(rootOp, llvm::ArrayRef<mlir::Value>{});
#endif
}

std::unique_ptr<mlir::Pass> createEqualitySaturationPass(const std::string& mlirFile, const std::string& eggFile, const EgglogCustomDefs& funcs) {
    return std::make_unique<EqualitySaturationPass>(mlirFile, eggFile, funcs);
}
