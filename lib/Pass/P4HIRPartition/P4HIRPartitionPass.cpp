// ============================================================================
// File: lib/Pass/P4HIRPartitionPass.cpp
// Complete P4HIR to vDSA Partitioning Pass - WITH EXIT TRACKING
// ============================================================================

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/JSON.h"

#include "Pass/P4HIRPartitionPass.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Ops.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"

#include <queue>
#include <fstream>
#include <set>

using namespace mlir;
using namespace P4::P4MLIR::P4HIR;

namespace {

// ============================================================================
// Exit Information - NEW!
// ============================================================================

struct ControlFlowExit {
  int exitId;
  int successorBlockId;
  std::string condition;  // "then", "else", "default"
};

// ============================================================================
// Dataflow Graph Representation
// ============================================================================

struct DataFlowNode {
  Operation *op;
  int blockId;
  std::string controlName;
  
  SmallVector<DataFlowNode*> dataSuccessors;
  SmallVector<DataFlowNode*> dataPredecessors;
  SmallVector<DataFlowNode*> controlSuccessors;
  SmallVector<DataFlowNode*> controlPredecessors;
  
  bool hasTableOp = false;
  bool hasMemoryOp = false;
  bool hasConditional = false;
  bool isPacketAccess = false;
  bool isNestedControlCall = false;
  
  bool canMapToVDRMT = true;
  bool canMapToVDPP = true;
  
  SmallVector<Value> liveIns;
  SmallVector<Value> liveOuts;
};

struct DataFlowGraph {
  SmallVector<DataFlowNode*> nodes;
  DenseMap<Operation*, DataFlowNode*> opToNode;
  DataFlowNode *entryNode = nullptr;
  DataFlowNode *exitNode = nullptr;
  
  ~DataFlowGraph() {
    for (auto node : nodes) {
      delete node;
    }
  }
};

// ============================================================================
// Basic Block - UPDATED WITH EXITS
// ============================================================================

struct BasicBlock {
  int blockId;
  std::string controlName;
  SmallVector<Operation*> operations;
  SmallVector<Value> liveIns;
  SmallVector<Value> liveOuts;
  
  SmallVector<BasicBlock*> predecessors;
  SmallVector<BasicBlock*> successors;
  
  // NEW: Exit tracking
  SmallVector<ControlFlowExit> exits;
  
  bool hasTableApply = false;
  bool hasMemoryAccess = false;
  bool hasConditionalBranch = false;
  bool isPacketProcessing = false;
  bool hasNestedControlCall = false;
  
  struct MappingCapability {
    bool canMapToVDRMT = true;
    bool canMapToVDPP = true;
    std::string reason;
  };
  
  MappingCapability capability;
  
  enum MemoryScope {
    NoMemory,
    LocalMemory,
    SharedPartitionable,
    SharedNonPartitionable
  };
  MemoryScope memoryScope = NoMemory;
  
  void analyze() {
    for (auto *op : operations) {
      if (isa<TableApplyOp>(op)) {
        hasTableApply = true;
      }
      if (auto callOp = dyn_cast<CallMethodOp>(op)) {
        auto methodName = callOp.getCallee().getLeafReference();
        if (methodName == "read" || methodName == "write") {
          hasMemoryAccess = true;
        }
      }
      if (isa<IfOp>(op)) {
        hasConditionalBranch = true;
      }
      if (isa<StructExtractOp, StructExtractRefOp>(op)) {
        isPacketProcessing = true;
      }
      if (isa<ApplyOp>(op)) {
        hasNestedControlCall = true;
      }
    }
    
    determineMapping();
  }
  
  void determineMapping() {
    capability.canMapToVDRMT = true;
    capability.canMapToVDPP = true;
    
    if (memoryScope == SharedNonPartitionable) {
      capability.canMapToVDRMT = false;
      capability.reason = "vDRMT does not support shared non-partitionable memory";
    }
  }
};

// ============================================================================
// Memory Analysis
// ============================================================================

class MemoryAnalyzer {
public:
  struct MemoryObject {
    Value memRef;
    std::string name;
    Type elementType;
    
    SmallVector<Operation*> readOps;
    SmallVector<Operation*> writeOps;
    SmallVector<Value> accessKeys;
    
    bool isLocal = false;
    bool isShared = false;
    bool isPartitionable = false;
    bool isPacketDerived = false;
    
    BasicBlock::MemoryScope getScope() const {
      if (isLocal) return BasicBlock::LocalMemory;
      if (isShared && isPartitionable) return BasicBlock::SharedPartitionable;
      if (isShared && !isPartitionable) return BasicBlock::SharedNonPartitionable;
      return BasicBlock::NoMemory;
    }
  };
  
  MemoryAnalyzer(ModuleOp module) : module(module) {}
  
  void analyze() {
    module.walk([&](InstantiateOp instOp) {
      auto calleeAttr = instOp->getAttrOfType<FlatSymbolRefAttr>("callee");
      if (calleeAttr && calleeAttr.getValue().contains("register")) {
        analyzeRegister(instOp);
      }
    });
  }
  
  const MemoryObject* getMemoryObject(Value memRef) const {
    auto it = memoryMap.find(memRef);
    return it != memoryMap.end() ? &it->second : nullptr;
  }
  
private:
  ModuleOp module;
  DenseMap<Value, MemoryObject> memoryMap;
  
  void analyzeRegister(InstantiateOp instOp) {
    MemoryObject memObj;
    memObj.memRef = instOp.getResult();
    memObj.name = instOp.getName().str();
    
    for (auto user : instOp.getResult().getUsers()) {
      if (auto callOp = dyn_cast<CallMethodOp>(user)) {
        auto methodName = callOp.getCallee().getLeafReference();
        if (methodName == "read") {
          memObj.readOps.push_back(callOp);
        } else if (methodName == "write") {
          memObj.writeOps.push_back(callOp);
        }
      }
    }
    
    if (memObj.readOps.size() + memObj.writeOps.size() <= 1) {
      memObj.isLocal = true;
    } else {
      memObj.isShared = true;
    }
    
    memoryMap[memObj.memRef] = memObj;
  }
};

// ============================================================================
// Dataflow Graph Builder
// ============================================================================

class DataFlowGraphBuilder {
public:
  DataFlowGraphBuilder(ModuleOp module, MemoryAnalyzer &memAnalyzer)
    : module(module), memAnalyzer(memAnalyzer), nextBlockId(0) {}
  
  std::unique_ptr<DataFlowGraph> build() {
    auto dfg = std::make_unique<DataFlowGraph>();
    
    llvm::outs() << "========================================\n";
    llvm::outs() << "Step 2: Building Dataflow Graph\n";
    llvm::outs() << "========================================\n\n";
    
    InstantiateOp mainInst = findMainInstantiation();
    if (!mainInst) {
      llvm::errs() << "Error: Could not find 'main' instantiation\n";
      return nullptr;
    }
    
    llvm::outs() << "✓ Found main: " << mainInst.getName() << "\n";
    
    auto calleeAttr = mainInst->getAttrOfType<FlatSymbolRefAttr>("callee");
    if (!calleeAttr) {
      llvm::errs() << "Error: main has no callee\n";
      return nullptr;
    }
    
    auto packageName = calleeAttr.getValue();
    auto numOperands = mainInst.getNumOperands();
    
    llvm::outs() << "  Package: " << packageName << "\n";
    llvm::outs() << "  Operands: " << numOperands << "\n";
    
    ControlOp mainControl;
    
    if (packageName == "NC_PIPELINE") {
      if (numOperands < 2) {
        llvm::errs() << "Error: NC_PIPELINE needs at least 2 operands\n";
        return nullptr;
      }
      
      llvm::outs() << "\n  NC_PIPELINE structure:\n";
      llvm::outs() << "    [0] = parser\n";
      llvm::outs() << "    [1] = control   <-- extracting this\n";
      llvm::outs() << "    [2] = deparser\n\n";
      
      Value controlOperand = mainInst.getOperand(1);
      mainControl = resolveOperandToControl(controlOperand);
      
    } else {
      llvm::outs() << "  Unknown package, searching by name...\n";
      
      module.walk([&](ControlOp ctrl) {
        if (ctrl.getSymName() == "MainControl") {
          mainControl = ctrl;
          return WalkResult::interrupt();
        }
        return WalkResult::advance();
      });
    }
    
    if (!mainControl) {
      llvm::errs() << "Error: Could not find main control\n";
      return nullptr;
    }
    
    llvm::outs() << "✓ Main control: " << mainControl.getSymName() << "\n\n";
    
    llvm::outs() << "Building DFG from MainControl...\n";
    buildFromControl(mainControl, dfg.get());
    
    llvm::outs() << "\n✓ DFG complete:\n";
    llvm::outs() << "  Total nodes: " << dfg->nodes.size() << "\n\n";
    
    return dfg;
  }

private:
  ModuleOp module;
  MemoryAnalyzer &memAnalyzer;
  int nextBlockId;
  
  InstantiateOp findMainInstantiation() {
    InstantiateOp mainInst;
    module.walk([&](InstantiateOp instOp) {
      if (instOp.getName() == "main") {
        mainInst = instOp;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    return mainInst;
  }
  
  ControlOp resolveOperandToControl(Value operand) {
    auto defOp = operand.getDefiningOp();
    if (!defOp) {
      llvm::errs() << "  Error: operand has no defining op\n";
      return nullptr;
    }
    
    if (auto instOp = dyn_cast<InstantiateOp>(defOp)) {
      llvm::outs() << "  Instance name: " << instOp.getName() << "\n";
      
      auto symRef = instOp->getAttrOfType<FlatSymbolRefAttr>("callee");
      if (!symRef) {
        llvm::errs() << "  Error: no callee attribute\n";
        return nullptr;
      }
      
      auto calleeName = symRef.getValue();
      llvm::outs() << "  Callee symbol: " << calleeName << "\n";
      
      return findControlByName(calleeName);
    }
    
    return nullptr;
  }
  
  ControlOp findControlByName(StringRef name) {
    ControlOp controlOp;
    module.walk([&](ControlOp ctrl) {
      if (ctrl.getSymName() == name) {
        controlOp = ctrl;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    return controlOp;
  }
  
  void buildFromControl(ControlOp controlOp, DataFlowGraph *dfg) {
    auto controlName = controlOp.getSymName().str();
    
    llvm::outs() << "  Processing control: " << controlName << "\n";
    
    auto &bodyRegion = controlOp.getBody();
    if (bodyRegion.empty()) {
      llvm::errs() << "    ERROR: Body region is empty\n";
      return;
    }
    
    Region *applyRegion = nullptr;
    
    for (auto &block : bodyRegion.getBlocks()) {
      for (auto &op : block.getOperations()) {
        if (op.getName().getStringRef() == "p4hir.control_apply") {
          applyRegion = &op.getRegion(0);
          break;
        }
      }
      if (applyRegion) break;
    }
    
    if (!applyRegion || applyRegion->empty()) {
      llvm::errs() << "    ERROR: No control_apply region found\n";
      return;
    }
    
    llvm::outs() << "    Found control_apply with " << applyRegion->getBlocks().size() 
                 << " blocks\n";
    
    dfg->entryNode = new DataFlowNode();
    dfg->entryNode->op = controlOp;
    dfg->entryNode->blockId = nextBlockId++;
    dfg->entryNode->controlName = controlName;
    dfg->nodes.push_back(dfg->entryNode);
    dfg->opToNode[controlOp] = dfg->entryNode;
    
    DataFlowNode *currentNode = dfg->entryNode;
    
    for (auto &block : applyRegion->getBlocks()) {
      llvm::outs() << "    Processing block with " << block.getOperations().size() 
                   << " operations\n";
      
      for (auto &op : block.getOperations()) {
        if (op.hasTrait<mlir::OpTrait::IsTerminator>()) {
          continue;
        }
        
        llvm::outs() << "      - " << op.getName() << "\n";
        
        auto node = createNodeForOp(&op, dfg, controlName);
        currentNode->controlSuccessors.push_back(node);
        node->controlPredecessors.push_back(currentNode);
        
        for (auto operand : op.getOperands()) {
          if (auto defOp = operand.getDefiningOp()) {
            if (auto defNode = dfg->opToNode.lookup(defOp)) {
              node->dataPredecessors.push_back(defNode);
              defNode->dataSuccessors.push_back(node);
            }
          }
        }
        
        currentNode = node;
      }
    }
    
    dfg->exitNode = new DataFlowNode();
    dfg->exitNode->blockId = nextBlockId++;
    dfg->exitNode->controlName = controlName + "::exit";
    dfg->nodes.push_back(dfg->exitNode);
    currentNode->controlSuccessors.push_back(dfg->exitNode);
    dfg->exitNode->controlPredecessors.push_back(currentNode);
  }
  
  DataFlowNode* createNodeForOp(Operation *op, DataFlowGraph *dfg, 
                                 const std::string& controlName) {
    auto node = new DataFlowNode();
    node->op = op;
    node->blockId = nextBlockId++;
    node->controlName = controlName;
    
    if (isa<TableApplyOp>(op)) {
      node->hasTableOp = true;
    }
    if (auto callOp = dyn_cast<CallMethodOp>(op)) {
      auto methodName = callOp.getCallee().getLeafReference();
      if (methodName == "read" || methodName == "write") {
        node->hasMemoryOp = true;
      }
    }
    if (isa<IfOp>(op)) {
      node->hasConditional = true;
    }
    if (isa<StructExtractOp, StructExtractRefOp>(op)) {
      node->isPacketAccess = true;
    }
    
    dfg->nodes.push_back(node);
    dfg->opToNode[op] = node;
    
    return node;
  }
};

// ============================================================================
// Control Flow Flattener - UPDATED WITH EXIT TRACKING
// ============================================================================

class ControlFlowFlattener {
public:
  ControlFlowFlattener(OpBuilder &builder, MemoryAnalyzer &memAnalyzer, ModuleOp module)
    : builder(builder), memAnalyzer(memAnalyzer), module(module) {}
  
  SmallVector<BasicBlock*> flattenMainControl() {
    SmallVector<BasicBlock*> allBlocks;
    
    ControlOp mainControl;
    
    module.walk([&](ControlOp controlOp) {
      auto controlName = controlOp.getSymName().str();
      llvm::outs() << "  Found control: " << controlName << "\n";
      
      if (controlName == "MainControl") {
        mainControl = controlOp;
        llvm::outs() << "    → This is MainControl, will flatten\n";
        return WalkResult::interrupt();
      } else {
        llvm::outs() << "    → Skipping (not MainControl)\n";
      }
      
      return WalkResult::advance();
    });
    
    if (!mainControl) {
      llvm::errs() << "  ERROR: MainControl not found!\n";
      return allBlocks;
    }
    
    llvm::outs() << "\n  Flattening MainControl...\n";
    auto blocks = flattenControl(mainControl);
    llvm::outs() << "    → Generated " << blocks.size() << " blocks\n\n";
    
    // NEW: Link successor IDs now that all blocks exist
    linkSuccessors(blocks);
    
    return blocks;
  }

private:
  OpBuilder &builder;
  MemoryAnalyzer &memAnalyzer;
  ModuleOp module;
  int nextBlockId = 0;
  
  // NEW: Track which blocks are successors
  DenseMap<BasicBlock*, SmallVector<BasicBlock*>> pendingSuccessors;
  
  SmallVector<BasicBlock*> flattenControl(ControlOp controlOp) {
    SmallVector<BasicBlock*> blocks;
    auto controlName = controlOp.getSymName().str();
    
    auto &bodyRegion = controlOp.getBody();
    if (bodyRegion.empty()) {
      llvm::errs() << "    ERROR: Body region is empty\n";
      return blocks;
    }
    
    Region *applyRegion = nullptr;
    
    for (auto &block : bodyRegion.getBlocks()) {
      for (auto &op : block.getOperations()) {
        if (op.getName().getStringRef() == "p4hir.control_apply") {
          llvm::outs() << "    Found p4hir.control_apply\n";
          applyRegion = &op.getRegion(0);
          break;
        }
      }
      if (applyRegion) break;
    }
    
    if (!applyRegion || applyRegion->empty()) {
      llvm::errs() << "    ERROR: No control_apply region found\n";
      return blocks;
    }
    
    llvm::outs() << "    control_apply region has " << applyRegion->getBlocks().size() 
                 << " blocks\n";
    
    for (auto &mlirBlock : applyRegion->getBlocks()) {
      llvm::outs() << "      Processing block with " << mlirBlock.getOperations().size() 
                   << " operations\n";
      flattenBlock(&mlirBlock, blocks, controlName);
    }
    
    return blocks;
  }
  
  void flattenBlock(Block *mlirBlock, SmallVector<BasicBlock*> &blocks, 
                    const std::string& controlName) {
    auto *bb = new BasicBlock();
    bb->blockId = nextBlockId++;
    bb->controlName = controlName;
    
    llvm::outs() << "        Creating basic block " << bb->blockId << "\n";
    
    for (auto &op : mlirBlock->getOperations()) {
      if (op.hasTrait<mlir::OpTrait::IsTerminator>()) {
        llvm::outs() << "          Skipping terminator: " << op.getName() << "\n";
        continue;
      }
      
      llvm::outs() << "          Processing: " << op.getName() << "\n";
      
      if (auto ifOp = dyn_cast<IfOp>(&op)) {
        llvm::outs() << "            → IfOp: INCLUDING it WITH condition\n";
        
        bb->operations.push_back(&op);
        bb->hasConditionalBranch = true;
        bb->analyze();
        blocks.push_back(bb);
        
        llvm::outs() << "              Created block " << bb->blockId 
                     << " with " << bb->operations.size() << " ops\n";
        
        // NEW: Flatten branches and track successors
        auto branchSuccessors = flattenIfBranches(ifOp, blocks, bb, controlName);
        pendingSuccessors[bb] = branchSuccessors;
        
        bb = new BasicBlock();
        bb->blockId = nextBlockId++;
        bb->controlName = controlName;
        
      } else if (auto tableApply = dyn_cast<TableApplyOp>(&op)) {
        llvm::outs() << "            → TableApplyOp\n";
        
        if (!bb->operations.empty()) {
          bb->analyze();
          blocks.push_back(bb);
          bb = new BasicBlock();
          bb->blockId = nextBlockId++;
          bb->controlName = controlName;
        }
        
        bb->operations.push_back(&op);
        bb->hasTableApply = true;
        bb->analyze();
        blocks.push_back(bb);
        
        bb = new BasicBlock();
        bb->blockId = nextBlockId++;
        bb->controlName = controlName;
        
      } else {
        llvm::outs() << "            → Adding to current block\n";
        bb->operations.push_back(&op);
      }
    }
    
    if (!bb->operations.empty()) {
      llvm::outs() << "        Final block " << bb->blockId << " has " 
                   << bb->operations.size() << " ops\n";
      bb->analyze();
      blocks.push_back(bb);
    } else {
      llvm::outs() << "        Final block is empty, deleting\n";
      delete bb;
    }
  }
  
  // NEW: Returns successors for linking later
  SmallVector<BasicBlock*> flattenIfBranches(IfOp ifOp, SmallVector<BasicBlock*> &blocks, 
                                              BasicBlock *conditionBB, const std::string& controlName) {
    SmallVector<BasicBlock*> successors;
    
    auto *thenBB = new BasicBlock();
    thenBB->blockId = nextBlockId++;
    thenBB->controlName = controlName;
    thenBB->predecessors.push_back(conditionBB);
    
    llvm::outs() << "              Processing then branch\n";
    
    for (auto &thenBlock : ifOp.getThenRegion().getBlocks()) {
      for (auto &op : thenBlock.getOperations()) {
        if (!op.hasTrait<mlir::OpTrait::IsTerminator>()) {
          llvm::outs() << "                - " << op.getName() << "\n";
          thenBB->operations.push_back(&op);
        }
      }
    }
    thenBB->analyze();
    blocks.push_back(thenBB);
    successors.push_back(thenBB);
    
    llvm::outs() << "              Created then block " << thenBB->blockId 
                 << " with " << thenBB->operations.size() << " ops\n";
    
    if (!ifOp.getElseRegion().empty()) {
      auto *elseBB = new BasicBlock();
      elseBB->blockId = nextBlockId++;
      elseBB->controlName = controlName;
      elseBB->predecessors.push_back(conditionBB);
      
      llvm::outs() << "              Processing else branch\n";
      
      for (auto &elseBlock : ifOp.getElseRegion().getBlocks()) {
        for (auto &op : elseBlock.getOperations()) {
          if (!op.hasTrait<mlir::OpTrait::IsTerminator>()) {
            llvm::outs() << "                - " << op.getName() << "\n";
            elseBB->operations.push_back(&op);
          }
        }
      }
      elseBB->analyze();
      blocks.push_back(elseBB);
      successors.push_back(elseBB);
      
      llvm::outs() << "              Created else block " << elseBB->blockId 
                   << " with " << elseBB->operations.size() << " ops\n";
    }
    
    return successors;
  }
  
  // NEW: Link successor block IDs after all blocks are created
  void linkSuccessors(SmallVector<BasicBlock*> &blocks) {
    llvm::outs() << "\n  Linking block successors...\n";
    
    for (auto *bb : blocks) {
      if (pendingSuccessors.count(bb)) {
        auto &successorBlocks = pendingSuccessors[bb];
        
        // Update the actual successor list
        bb->successors = successorBlocks;
        
        // Create exit info
        int exitId = 0;
        for (auto *succ : successorBlocks) {
          ControlFlowExit exit;
          exit.exitId = exitId;
          exit.successorBlockId = succ->blockId;
          exit.condition = (exitId == 0) ? "then" : "else";
          
          bb->exits.push_back(exit);
          
          llvm::outs() << "    Block " << bb->blockId << " exit " << exitId 
                       << " → Block " << succ->blockId << " (" << exit.condition << ")\n";
          
          exitId++;
        }
      } else if (!bb->operations.empty()) {
        // No explicit successors = end of pipeline
        ControlFlowExit exit;
        exit.exitId = 0;
        exit.successorBlockId = -1;  // -1 = end of pipeline
        exit.condition = "default";
        bb->exits.push_back(exit);
        
        llvm::outs() << "    Block " << bb->blockId << " exit 0 → END\n";
      }
    }
  }
};

// ============================================================================
// Multi-Target Code Generator - UPDATED TO WRITE EXITS
// ============================================================================

class MultiTargetCodeGen {
public:
  MultiTargetCodeGen(MLIRContext *context, ModuleOp originalModule, StringRef outputDir)
    : context(context), originalModule(originalModule), outputDir(outputDir.str()) {}
  
  LogicalResult generateAll(SmallVector<BasicBlock*> &blocks) {
    if (llvm::sys::fs::create_directories(outputDir)) {
      llvm::errs() << "Failed to create directory: " << outputDir << "\n";
      return failure();
    }
    
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (failed(generateCode(blocks[i], i))) {
        return failure();
      }
    }
    
    if (failed(generateDOT(blocks))) {
      return failure();
    }
    
    return success();
  }

private:
  MLIRContext *context;
  ModuleOp originalModule;
  std::string outputDir;
  
  ControlOp getControlOp() {
    ControlOp controlOp;
    originalModule.walk([&](ControlOp ctrl) {
      if (ctrl.getSymName() == "MainControl") {
        controlOp = ctrl;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    return controlOp;
  }
  
  LogicalResult generateCode(BasicBlock *bb, int blockNum) {
    std::string blockDir = outputDir + "/block" + std::to_string(blockNum);
    if (llvm::sys::fs::create_directories(blockDir)) {
      llvm::errs() << "Failed to create directory: " << blockDir << "\n";
      return failure();
    }
    
    if (failed(writeP4HIR(bb, blockDir, blockNum))) return failure();
    if (failed(writeMetadata(bb, blockDir))) return failure();
    
    return success();
  }
    
  LogicalResult writeP4HIR(BasicBlock *bb, StringRef blockDir, int blockNum) {
    std::string filename = (blockDir + "/p4hir.mlir").str();
    std::error_code ec;
    llvm::raw_fd_ostream file(filename, ec);
    
    if (ec) {
      llvm::errs() << "Failed to open: " << filename << "\n";
      return failure();
    }
    
    auto originalControl = getControlOp();
    if (!originalControl) {
      llvm::errs() << "Failed to find MainControl\n";
      return failure();
    }
    
    auto module = ModuleOp::create(UnknownLoc::get(context));
    OpBuilder builder(context);
    builder.setInsertionPointToStart(module.getBody());
    
    SmallVector<Type> argTypes;
    for (auto arg : originalControl.getArguments()) {
      argTypes.push_back(arg.getType());
    }
    
    auto funcType = builder.getFunctionType(argTypes, {});
    auto func = builder.create<func::FuncOp>(
      UnknownLoc::get(context),
      "block" + std::to_string(blockNum),
      funcType
    );
    
    for (unsigned i = 0; i < originalControl.getNumArguments(); ++i) {
      auto argAttrs = originalControl.getArgAttrDict(i);
      if (argAttrs) {
        func.setArgAttrs(i, argAttrs);
      }
    }
    
    Block *funcBody = func.addEntryBlock();
    builder.setInsertionPointToStart(funcBody);
    
    IRMapping mapping;
    for (unsigned i = 0; i < originalControl.getNumArguments(); ++i) {
      mapping.map(originalControl.getArgument(i), funcBody->getArgument(i));
    }
    
    for (auto *op : bb->operations) {
      if (auto ifOp = dyn_cast<IfOp>(op)) {
        Value condition = ifOp.getCondition();
        Value mappedCondition = mapping.lookupOrDefault(condition);
        
        bool hasElse = !ifOp.getElseRegion().empty();
        
        auto emptyBuilder = [](OpBuilder &b, Location loc) {
          b.create<YieldOp>(loc);
        };
        
        if (hasElse) {
          builder.create<IfOp>(
            UnknownLoc::get(context),
            mappedCondition,
            true,
            emptyBuilder,
            DictionaryAttr(),
            emptyBuilder,
            DictionaryAttr()
          );
        } else {
          builder.create<IfOp>(
            UnknownLoc::get(context),
            mappedCondition,
            false,
            emptyBuilder,
            DictionaryAttr()
          );
        }
        
      } else {
        Operation *cloned = builder.clone(*op, mapping);
        
        for (unsigned i = 0; i < op->getNumResults(); ++i) {
          mapping.map(op->getResult(i), cloned->getResult(i));
        }
      }
    }
    
    builder.create<func::ReturnOp>(UnknownLoc::get(context));
    
    OpPrintingFlags flags;
    flags.printGenericOpForm(false);
    flags.enableDebugInfo(false, false);
    
    module.print(file, flags);
    module.erase();
    
    return success();
  }
  
  LogicalResult writeMetadata(BasicBlock *bb, StringRef blockDir) {
    std::string filename = (blockDir + "/metadata.json").str();
    std::error_code ec;
    llvm::raw_fd_ostream file(filename, ec);
    
    if (ec) return failure();
    
    llvm::json::Object meta;
    meta["blockId"] = bb->blockId;
    meta["controlName"] = bb->controlName;
    meta["operations"] = (int64_t)bb->operations.size();
    meta["hasTableApply"] = bb->hasTableApply;
    meta["hasMemoryAccess"] = bb->hasMemoryAccess;
    meta["hasConditionalBranch"] = bb->hasConditionalBranch;
    meta["hasNestedControlCall"] = bb->hasNestedControlCall;
    meta["canMapToVDRMT"] = bb->capability.canMapToVDRMT;
    meta["canMapToVDPP"] = bb->capability.canMapToVDPP;
    
    // NEW: Add exits array
    llvm::json::Array exitsArray;
    for (auto &exit : bb->exits) {
      llvm::json::Object exitObj;
      exitObj["exitId"] = exit.exitId;
      exitObj["successor"] = exit.successorBlockId;
      exitObj["condition"] = exit.condition;
      exitsArray.push_back(std::move(exitObj));
    }
    meta["exits"] = std::move(exitsArray);
    
    if (!bb->capability.reason.empty()) {
      meta["reason"] = bb->capability.reason;
    }
    
    std::string memoryScope;
    switch (bb->memoryScope) {
      case BasicBlock::NoMemory: memoryScope = "none"; break;
      case BasicBlock::LocalMemory: memoryScope = "local"; break;
      case BasicBlock::SharedPartitionable: memoryScope = "shared_partitionable"; break;
      case BasicBlock::SharedNonPartitionable: memoryScope = "shared_non_partitionable"; break;
    }
    meta["memoryScope"] = memoryScope;
    
    meta["predecessors"] = (int64_t)bb->predecessors.size();
    meta["successors"] = (int64_t)bb->successors.size();
    
    llvm::json::Array opsArray;
    for (size_t i = 0; i < bb->operations.size(); ++i) {
      auto *op = bb->operations[i];
      llvm::json::Object opObj;
      opObj["index"] = (int64_t)i;
      opObj["name"] = op->getName().getStringRef().str();
      opObj["numOperands"] = (int64_t)op->getNumOperands();
      opObj["numResults"] = (int64_t)op->getNumResults();
      opsArray.push_back(std::move(opObj));
    }
    meta["operationDetails"] = std::move(opsArray);
    
    file << llvm::formatv("{0:2}", llvm::json::Value(std::move(meta)));
    
    return success();
  }
  
  LogicalResult generateDOT(SmallVector<BasicBlock*> &blocks) {
    std::string filename = outputDir + "/dependency_graph.dot";
    std::error_code ec;
    llvm::raw_fd_ostream file(filename, ec);
    
    if (ec) return failure();
    
    file << "digraph ControlFlowGraph {\n";
    file << "  rankdir=TB;\n";
    file << "  node [shape=box, style=\"rounded,filled\", fontname=\"monospace\"];\n";
    file << "  edge [fontname=\"monospace\", fontsize=10];\n\n";
    
    for (auto *bb : blocks) {
      file << "  block" << bb->blockId << " [";
      
      if (bb->hasConditionalBranch) {
        file << "fillcolor=\"lightyellow\", ";
      } else if (bb->hasTableApply) {
        file << "fillcolor=\"lightblue\", ";
      } else if (bb->hasMemoryAccess) {
        file << "fillcolor=\"lightgreen\", ";
      } else {
        file << "fillcolor=\"white\", ";
      }
      
      file << "label=\"Block " << bb->blockId << "\\n━━━━━━━━━━━━━━━━━━\\n";
      
      int opIdx = 0;
      for (auto *op : bb->operations) {
        std::string opName = op->getName().getStringRef().str();
        if (opName.find("p4hir.") == 0) {
          opName = opName.substr(6);
        }
        file << opIdx << ": " << opName << "\\l";
        opIdx++;
      }
      
      file << "━━━━━━━━━━━━━━━━━━\\nTotal: " << bb->operations.size() << " ops\\l";
      
      if (bb->hasConditionalBranch) file << "[BRANCH]\\l";
      if (bb->hasTableApply) file << "[TABLE]\\l";
      if (bb->hasMemoryAccess) file << "[MEMORY]\\l";
      
      file << "\\nvDRMT: " << (bb->capability.canMapToVDRMT ? "✓" : "✗");
      file << " | vDPP: " << (bb->capability.canMapToVDPP ? "✓" : "✗") << "\\l";
      
      file << "\"];\n";
    }
    
    file << "\n";
    
    for (auto *bb : blocks) {
      for (auto &exit : bb->exits) {
        if (exit.successorBlockId >= 0) {
          file << "  block" << bb->blockId << " -> block" << exit.successorBlockId;
          file << " [label=\"" << exit.condition << "\"";
          if (exit.condition == "then") {
            file << ", color=\"green\"";
          } else if (exit.condition == "else") {
            file << ", color=\"red\"";
          }
          file << "];\n";
        }
      }
    }
    
    file << "\n  legend [shape=box, style=\"filled\", fillcolor=\"lightgray\", label=\"";
    file << "Legend:\\lYellow = Conditional\\lBlue = Table\\lGreen = Memory\\l\"];\n";
    file << "}\n";
    
    llvm::outs() << "  Generated DOT: " << filename << "\n";
    
    return success();
  }
};

// ============================================================================
// Main Pass
// ============================================================================

struct P4HIRPartitionPass 
  : public PassWrapper<P4HIRPartitionPass, OperationPass<ModuleOp>> {
  
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(P4HIRPartitionPass)
  
  P4HIRPartitionPass() = default;
  P4HIRPartitionPass(const P4HIRPartitionPass &other) : PassWrapper(other) {}
  
  Option<std::string> outputDirOption{
    *this, 
    "output-dir",
    llvm::cl::desc("Output directory"),
    llvm::cl::init("vdsa_output")
  };
  
  StringRef getArgument() const final { return "p4hir-partition"; }
  StringRef getDescription() const final {
    return "Partition P4HIR and generate multi-target vDSA code with exit tracking";
  }
  
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<vdrmt::vDRMTDialect>();
    registry.insert<vdpp::vDPPDialect>();
    registry.insert<func::FuncDialect>();
  }
  
  void runOnOperation() override {
    auto module = getOperation();
    auto *context = &getContext();
    std::string outputDir = outputDirOption.getValue();
    
    llvm::outs() << "\n╔════════════════════════════════════════╗\n";
    llvm::outs() << "║  P4HIR to vDSA Partition Pass         ║\n";
    llvm::outs() << "╚════════════════════════════════════════╝\n\n";
    
    llvm::outs() << "Step 1: Analyzing memory objects...\n";
    llvm::outs() << "========================================\n";
    MemoryAnalyzer memAnalyzer(module);
    memAnalyzer.analyze();
    llvm::outs() << "✓ Complete\n\n";
    
    DataFlowGraphBuilder dfgBuilder(module, memAnalyzer);
    auto dfg = dfgBuilder.build();
    
    if (!dfg) {
      llvm::errs() << "✗ Failed to build dataflow graph\n";
      signalPassFailure();
      return;
    }
    
    llvm::outs() << "Step 3: Flattening control flow...\n";
    llvm::outs() << "========================================\n";
    OpBuilder builder(context);
    ControlFlowFlattener flattener(builder, memAnalyzer, module);
    
    auto blocks = flattener.flattenMainControl();
    
    llvm::outs() << "✓ Flattened into " << blocks.size() << " basic blocks\n\n";
    
    llvm::outs() << "Step 4: Generating multi-target code...\n";
    llvm::outs() << "========================================\n";
    
    if (blocks.size() == 0) {
      llvm::errs() << "✗ No blocks generated\n";
      signalPassFailure();
      return;
    }
    
    MultiTargetCodeGen codeGen(context, module, outputDir);
    
    if (failed(codeGen.generateAll(blocks))) {
      llvm::errs() << "✗ Code generation failed\n";
      signalPassFailure();
      return;
    }
    
    llvm::outs() << "\n╔════════════════════════════════════════╗\n";
    llvm::outs() << "║  ✓ Code Generation Complete!          ║\n";
    llvm::outs() << "╚════════════════════════════════════════╝\n\n";
    llvm::outs() << "Output: " << outputDir << "/\n";
    llvm::outs() << "  ├─ dependency_graph.dot\n";
    for (size_t i = 0; i < blocks.size(); ++i) {
      llvm::outs() << "  ├─ block" << i << "/\n";
      llvm::outs() << "  │  ├─ p4hir.mlir\n";
      llvm::outs() << "  │  └─ metadata.json (with exits)\n";
    }
    llvm::outs() << "\n";
    
    for (auto *bb : blocks) {
      delete bb;
    }
  }
};

} // anonymous namespace

namespace mlir {

std::unique_ptr<Pass> createP4HIRPartitionPass() {
  return std::make_unique<P4HIRPartitionPass>();
}

void registerP4HIRPartitionPass() {
  PassRegistration<P4HIRPartitionPass>();
}

} // namespace mlir