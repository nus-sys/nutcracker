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
// Packet-derivation check (P4HIR level)
// ============================================================================

/// Return false if v's def-chain contains a stateful read (call_method whose
/// leaf name is "read" or "execute").  Block arguments — which represent
/// hdr/meta values at block boundaries — are conservatively packet-derived.
///
/// This is intentionally shallow: we trace through arithmetic/cast/extract
/// ops but not through memory references (load/store on struct fields).  That
/// keeps the analysis sound for the common case and is the "simple" starting
/// point the architecture calls for.
static bool isPacketDerivedP4HIR(Value v, DenseSet<Value> &visited) {
    if (!visited.insert(v).second) return true; // cycle guard
    Operation *def = v.getDefiningOp();
    if (!def) return true; // block argument → conservatively packet-derived
    if (auto callOp = dyn_cast<CallMethodOp>(def)) {
        StringRef leaf = callOp.getCallee().getLeafReference();
        // "read"    — register / direct stateful read
        // "execute" — meter execute, returns color derived from state
        if (leaf == "read" || leaf == "execute")
            return false;
        // Other call_methods (hash5tuple.apply, count, …) are pure
        // packet-function calls and do not introduce state dependency.
    }
    for (Value operand : def->getOperands())
        if (!isPacketDerivedP4HIR(operand, visited))
            return false;
    return true;
}

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
  llvm::SmallPtrSet<Operation*, 4> branchPointIfOps; // IfOps whose branches became separate blocks
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
  bool setsDropBit = false;  // writes standard_meta.drop = 1
  bool isTerminal = false;   // synthetic forward/drop terminal block
  
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
    // Collect all ops including those nested inside IfOp regions.
    SmallVector<Operation *> allOps;
    for (auto *op : operations) {
      allOps.push_back(op);
      op->walk([&](Operation *nested) {
        if (nested != op) allOps.push_back(nested);
      });
    }

    // Track which Values are refs to the "drop" field so we can detect writes.
    llvm::SmallPtrSet<Value, 4> dropRefs;

    for (auto *op : allOps) {
      if (isa<TableApplyOp>(op))
        hasTableApply = true;
      if (auto callOp = dyn_cast<CallMethodOp>(op)) {
        auto methodName = callOp.getCallee().getLeafReference();
        if (methodName == "read" || methodName == "write")
          hasMemoryAccess = true;
      }
      if (isa<IfOp>(op))
        hasConditionalBranch = true;
      if (isa<StructExtractOp, StructExtractRefOp>(op))
        isPacketProcessing = true;
      if (isa<ApplyOp>(op))
        hasNestedControlCall = true;

      // Detect: p4hir.struct_extract_ref %standard_meta['drop']
      if (auto extractRef = dyn_cast<StructExtractRefOp>(op)) {
        if (extractRef.getFieldName() == "drop")
          dropRefs.insert(extractRef.getResult());
      }
      // Detect: p4hir.assign %const_1 → drop_ref
      if (auto assignOp = dyn_cast<AssignOp>(op)) {
        if (dropRefs.contains(assignOp.getRef())) {
          // Any assignment to the drop field counts — the constant value is
          // always 1 in well-formed P4 (you only set drop, never clear it).
          setsDropBit = true;
        }
      }
    }

    determineMapping();
  }
  
  /// Return false if any stateful call_method in `root` has a non-packet-derived
  /// index argument.  Used to check both direct ops and callee bodies.
  static bool statefulOpsAllPacketDerived(Operation *root) {
    bool ok = true;
    root->walk([&](CallMethodOp callOp) {
      StringRef leaf = callOp.getCallee().getLeafReference();
      if (leaf != "read" && leaf != "write" && leaf != "execute")
        return WalkResult::advance();
      auto args = callOp.getArgOperands();
      if (args.empty()) return WalkResult::advance();
      DenseSet<Value> visited;
      if (!isPacketDerivedP4HIR(args[0], visited)) {
        ok = false;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    return ok;
  }

  void determineMapping() {
    // bf3drmt is the concrete target for vDRMT blocks.  It handles counter
    // and meter sharing natively in hardware, and uses hardware-level packet
    // distribution for registers.  So vDRMT is always a valid target.
    capability.canMapToVDRMT = true;

    // bf3dpa is the concrete target for vDPP blocks (ARM cores).  Multiple
    // cores run in parallel with no hardware packet-sharding assist, so any
    // stateful op whose index is NOT derived from packet fields alone could
    // produce conflicting accesses across cores.
    //
    // Simple rule: if any stateful op (register read/write, meter execute) in
    // this block (or in a directly called P4 action) uses a non-packet-derived
    // index, the block cannot map to vDPP.
    capability.canMapToVDPP = true;

    if (operations.empty()) return;

    // Find the enclosing ControlOp so we can resolve p4hir.call callees.
    ControlOp enclosingControl =
        operations[0]->getParentOfType<ControlOp>();

    // Flatten nested ops (IfOp regions, etc.) for the scan.
    SmallVector<Operation *> allOps;
    for (auto *op : operations) {
      allOps.push_back(op);
      op->walk([&](Operation *nested) {
        if (nested != op) allOps.push_back(nested);
      });
    }

    for (auto *op : allOps) {
      // ── Direct stateful call_method ────────────────────────────────────────
      if (auto callOp = dyn_cast<CallMethodOp>(op)) {
        StringRef leaf = callOp.getCallee().getLeafReference();
        if (leaf == "read" || leaf == "write" || leaf == "execute") {
          auto args = callOp.getArgOperands();
          if (!args.empty()) {
            DenseSet<Value> visited;
            if (!isPacketDerivedP4HIR(args[0], visited)) {
              capability.canMapToVDPP = false;
              capability.reason =
                  "stateful op index not packet-derived; "
                  "bf3dpa requires packet-shardable indices";
              return;
            }
          }
        }
        continue;
      }

      // ── Indirect: p4hir.call @action() — resolve one level ────────────────
      // P4 actions are declared as p4hir.func inside the enclosing control.
      // Walk the control to find the body and apply the same check.
      if (auto callOp = dyn_cast<CallOp>(op)) {
        if (!enclosingControl) continue;
        StringRef callee = callOp.getCallee();
        FuncOp actionFunc;
        enclosingControl.walk([&](FuncOp f) {
          if (f.getSymName() == callee) {
            actionFunc = f;
            return WalkResult::interrupt();
          }
          return WalkResult::advance();
        });
        if (actionFunc && !statefulOpsAllPacketDerived(actionFunc)) {
          capability.canMapToVDPP = false;
          capability.reason =
              "stateful op in called action has non-packet-derived index; "
              "bf3dpa requires packet-shardable indices";
          return;
        }
      }
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
    
    // Link conditional/fallthrough successors now that all blocks exist.
    linkSuccessors(blocks);

    // Append synthetic terminal blocks and wire all pipeline-exit leaves to them.
    appendTerminalBlocks(blocks);

    return blocks;
  }

private:
  OpBuilder &builder;
  MemoryAnalyzer &memAnalyzer;
  ModuleOp module;
  int nextBlockId = 0;
  
  // Conditional successors: if-condition block → [then-entry, else-entry]
  DenseMap<BasicBlock*, SmallVector<BasicBlock*>> pendingSuccessors;
  // Fall-through successors: table block → index in blocks[] of the next block
  DenseMap<BasicBlock*, size_t> pendingFallthroughIdx;
  // Implicit else: no-else if-condition block → index in blocks[] where
  // continuation starts (or past-end if nothing follows → wire to FORWARD).
  DenseMap<BasicBlock*, size_t> pendingImplicitElseIdx;
  
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
        bb->branchPointIfOps.insert(&op); // mark as partitioned branch point
        bb->hasConditionalBranch = true;
        bb->analyze();
        blocks.push_back(bb);

        llvm::outs() << "              Created block " << bb->blockId
                     << " with " << bb->operations.size() << " ops\n";

        // Flatten branches and track successors.
        auto branchSuccessors = flattenIfBranches(ifOp, blocks, bb, controlName);
        pendingSuccessors[bb] = branchSuccessors;

        // If the P4 source had no else branch, record the index where the
        // continuation block will be created.  appendTerminalBlocks() uses
        // this to wire the implicit else edge (→ continuation or → FORWARD).
        BasicBlock *condBB = bb;
        bb = new BasicBlock();
        bb->blockId = nextBlockId++;
        bb->controlName = controlName;
        if (ifOp.getElseRegion().empty())
          pendingImplicitElseIdx[condBB] = blocks.size();
        
      } else if (auto tableApply = dyn_cast<TableApplyOp>(&op)) {
        llvm::outs() << "            → TableApplyOp\n";

        if (!bb->operations.empty()) {
          bb->analyze();
          blocks.push_back(bb);
          // Pre-table ops block falls through to the table block (pushed next).
          pendingFallthroughIdx[bb] = blocks.size();
          bb = new BasicBlock();
          bb->blockId = nextBlockId++;
          bb->controlName = controlName;
        }

        bb->operations.push_back(&op);
        bb->hasTableApply = true;
        bb->analyze();
        blocks.push_back(bb);
        // Table block falls through to the first block created after it.
        pendingFallthroughIdx[bb] = blocks.size();

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

    // ── then branch ──────────────────────────────────────────────────────────
    llvm::outs() << "              Processing then branch (recursive)\n";
    size_t beforeThen = blocks.size();
    for (auto &thenBlock : ifOp.getThenRegion().getBlocks())
      flattenBlock(&thenBlock, blocks, controlName);

    if (blocks.size() > beforeThen) {
      BasicBlock *thenEntry = blocks[beforeThen];
      thenEntry->predecessors.push_back(conditionBB);
      successors.push_back(thenEntry);
      llvm::outs() << "              Then entry block " << thenEntry->blockId
                   << " (" << (blocks.size() - beforeThen) << " blocks total)\n";
    }

    // ── else branch (optional) ────────────────────────────────────────────────
    if (!ifOp.getElseRegion().empty()) {
      llvm::outs() << "              Processing else branch (recursive)\n";
      size_t beforeElse = blocks.size();
      for (auto &elseBlock : ifOp.getElseRegion().getBlocks())
        flattenBlock(&elseBlock, blocks, controlName);

      if (blocks.size() > beforeElse) {
        BasicBlock *elseEntry = blocks[beforeElse];
        elseEntry->predecessors.push_back(conditionBB);
        successors.push_back(elseEntry);
        llvm::outs() << "              Else entry block " << elseEntry->blockId
                     << " (" << (blocks.size() - beforeElse) << " blocks total)\n";
      }
    }

    return successors;
  }
  
  void linkSuccessors(SmallVector<BasicBlock*> &blocks) {
    llvm::outs() << "\n  Linking block successors...\n";

    for (auto *bb : blocks) {
      if (pendingSuccessors.count(bb)) {
        // If-condition block: then/else branch exits.
        auto &successorBlocks = pendingSuccessors[bb];
        bb->successors = successorBlocks;
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
      } else if (pendingFallthroughIdx.count(bb)) {
        // Table block (or pre-table block): single fall-through exit.
        size_t idx = pendingFallthroughIdx[bb];
        if (idx < blocks.size()) {
          BasicBlock *dst = blocks[idx];
          bb->successors.push_back(dst);
          ControlFlowExit exit;
          exit.exitId = 0;
          exit.successorBlockId = dst->blockId;
          exit.condition = "default";
          bb->exits.push_back(exit);
          llvm::outs() << "    Block " << bb->blockId << " fallthrough → Block "
                       << dst->blockId << "\n";
        } else {
          // Table is the last block in its branch → end of pipeline.
          ControlFlowExit exit;
          exit.exitId = 0;
          exit.successorBlockId = -1;
          exit.condition = "default";
          bb->exits.push_back(exit);
          llvm::outs() << "    Block " << bb->blockId << " fallthrough → END\n";
        }
      } else if (!bb->operations.empty()) {
        // Leaf block with no recorded successors → end of pipeline.
        ControlFlowExit exit;
        exit.exitId = 0;
        exit.successorBlockId = -1;
        exit.condition = "default";
        bb->exits.push_back(exit);
        llvm::outs() << "    Block " << bb->blockId << " exit 0 → END\n";
      }
    }
  }

  // Create synthetic forward/drop terminal blocks.  Every leaf that currently
  // has successorBlockId == -1 is rewired to the appropriate terminal.
  void appendTerminalBlocks(SmallVector<BasicBlock*> &blocks) {
    // Build the two terminals.
    auto *fwdBlock = new BasicBlock();
    fwdBlock->blockId = nextBlockId++;
    fwdBlock->controlName = "pipeline_exit";
    fwdBlock->isTerminal = true;

    auto *dropBlock = new BasicBlock();
    dropBlock->blockId = nextBlockId++;
    dropBlock->controlName = "pipeline_exit";
    dropBlock->isTerminal = true;

    // Wire leaves to the appropriate terminal and update their exit records.
    bool anyDrop = false;
    for (auto *bb : blocks) {
      for (auto &exit : bb->exits) {
        if (exit.successorBlockId != -1) continue;
        BasicBlock *terminal = bb->setsDropBit ? dropBlock : fwdBlock;
        exit.successorBlockId = terminal->blockId;
        exit.condition = bb->setsDropBit ? "drop" : "forward";
        bb->successors.push_back(terminal);
        terminal->predecessors.push_back(bb);
        if (bb->setsDropBit) anyDrop = true;
        llvm::outs() << "    Block " << bb->blockId << " → terminal "
                     << exit.condition << " (block " << terminal->blockId << ")\n";
      }
    }

    // Wire implicit else edges for no-else if-condition blocks.
    // At this point blocks[] contains all pipeline blocks but NOT the terminals
    // yet, so idx >= blocks.size() reliably means "nothing follows → forward".
    for (auto &[condBB, idx] : pendingImplicitElseIdx) {
      BasicBlock *elseDst = (idx < blocks.size()) ? blocks[idx] : fwdBlock;
      condBB->successors.push_back(elseDst);
      ControlFlowExit exit;
      exit.exitId = (int)condBB->exits.size();
      exit.successorBlockId = elseDst->blockId;
      exit.condition = "else";
      condBB->exits.push_back(exit);
      elseDst->predecessors.push_back(condBB);
      llvm::outs() << "    Block " << condBB->blockId << " implicit else → Block "
                   << elseDst->blockId << "\n";
    }

    blocks.push_back(fwdBlock);
    if (anyDrop)
      blocks.push_back(dropBlock);
    else
      delete dropBlock;

    llvm::outs() << "    Added terminal block: forward (block " << fwdBlock->blockId << ")\n";
    if (anyDrop)
      llvm::outs() << "    Added terminal block: drop (block " << dropBlock->blockId << ")\n";
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

    // Terminal blocks have no ops — only emit metadata.
    if (!bb->isTerminal) {
      if (failed(writeP4HIR(bb, blockDir, blockNum))) return failure();
    }
    if (failed(writeMetadata(bb, blockDir))) return failure();

    return success();
  }
    
  // Ensure that 'v' (a value defined in the original control) is available in
  // 'mapping'. If not, recursively clone its defining op (and all its
  // transitive dependencies) using 'builder'.
  void ensureAvailable(Value v, IRMapping &mapping, OpBuilder &builder) {
    if (mapping.contains(v)) return;
    Operation *defOp = v.getDefiningOp();
    if (!defOp) return; // block argument — should already be mapped
    for (auto operand : defOp->getOperands())
      ensureAvailable(operand, mapping, builder);
    Operation *cloned = builder.clone(*defOp, mapping);
    for (unsigned i = 0; i < defOp->getNumResults(); ++i)
      mapping.map(defOp->getResult(i), cloned->getResult(i));
  }

  // Walk 'root' looking for p4hir.call ops whose callee is an action in
  // 'originalControl'. For each one, inline the action body in-place (inserting
  // ops immediately before the call) and erase the call.
  void inlineActionCalls(Operation *root, ControlOp originalControl,
                         IRMapping &outerMapping) {
    // Collect calls first (walk + erase is unsafe).
    SmallVector<CallOp> calls;
    root->walk([&](CallOp c) { calls.push_back(c); });

    for (auto callOp : calls) {
      StringRef actionName = callOp.getCallee();

      // Find the matching action FuncOp in the original control.
      FuncOp actionFunc;
      originalControl.walk([&](FuncOp f) {
        if (f.getSymName() == actionName) {
          actionFunc = f;
          return WalkResult::interrupt();
        }
        return WalkResult::advance();
      });
      if (!actionFunc || actionFunc.getBody().empty()) continue;

      // Insert action body ops immediately before the call.
      OpBuilder ab(callOp);
      IRMapping actionMapping(outerMapping); // inherits control-arg mappings

      Block &entry = actionFunc.getBody().front();
      for (auto &aOp : entry.getOperations()) {
        if (aOp.hasTrait<OpTrait::IsTerminator>()) break;
        // Ensure every operand (potentially from outer control scope) is mapped.
        for (auto operand : aOp.getOperands())
          ensureAvailable(operand, actionMapping, ab);
        Operation *inlined = ab.clone(aOp, actionMapping);
        for (unsigned i = 0; i < aOp.getNumResults(); ++i)
          actionMapping.map(aOp.getResult(i), inlined->getResult(i));
      }
      callOp.erase();
    }
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
        if (bb->branchPointIfOps.count(op)) {
          // This IfOp was partitioned: its branches became separate blocks.
          // Reconstruct with empty bodies (condition-only marker).
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
          // Inline IfOp (action-level control flow): clone with full content,
          // then inline any p4hir.call to actions so the output module is
          // self-contained (no dangling callee references).
          Operation *cloned = builder.clone(*op, mapping);
          inlineActionCalls(cloned, originalControl, mapping);
          for (unsigned i = 0; i < op->getNumResults(); ++i) {
            mapping.map(op->getResult(i), cloned->getResult(i));
          }
        }

      } else {
        // Ensure all values used in this op's subtree (including nested
        // regions) that are defined outside this op are available in mapping.
        // This covers extern instances (e.g. p4hir.instantiate @Hash5Tuple)
        // defined at control scope but referenced inside p4hir.scope regions.
        op->walk([&](Operation *nested) {
          for (auto operand : nested->getOperands()) {
            Operation *defOp = operand.getDefiningOp();
            if (!defOp) continue; // block argument, already mapped
            if (!op->isAncestor(defOp))
              ensureAvailable(operand, mapping, builder);
          }
        });
        Operation *cloned = builder.clone(*op, mapping);

        for (unsigned i = 0; i < op->getNumResults(); ++i) {
          mapping.map(op->getResult(i), cloned->getResult(i));
        }
      }
    }
    
    // Inline any top-level action calls that weren't inside an IfOp
    // (e.g. leaf blocks that contain only a bare action call like count_syn()).
    inlineActionCalls(func, originalControl, mapping);

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
    meta["isTerminal"] = bb->isTerminal;
    meta["setsDropBit"] = bb->setsDropBit;
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

      if (bb->isTerminal) {
        // Terminal blocks: double-octagon shape, color by action type.
        file << "shape=doubleoctagon, ";
        file << (bb->setsDropBit ? "fillcolor=\"salmon\"" : "fillcolor=\"lightgreen\"") << ", ";
      } else if (bb->hasConditionalBranch) {
        file << "fillcolor=\"lightyellow\", ";
      } else if (bb->hasTableApply) {
        file << "fillcolor=\"lightblue\", ";
      } else if (bb->hasMemoryAccess) {
        file << "fillcolor=\"lightgreen\", ";
      } else {
        file << "fillcolor=\"white\", ";
      }

      if (bb->isTerminal) {
        // Compact label for terminal blocks.
        std::string action = bb->setsDropBit ? "DROP" : "FORWARD";
        file << "label=\"" << action << "\\nBlock " << bb->blockId << "\"";
      } else {
        file << "label=\"Block " << bb->blockId << "\\n━━━━━━━━━━━━━━━━━━\\n";

        int opIdx = 0;
        for (auto *op : bb->operations) {
          std::string opName = op->getName().getStringRef().str();
          if (opName.find("p4hir.") == 0)
            opName = opName.substr(6);
          file << opIdx << ": " << opName << "\\l";
          opIdx++;
        }

        file << "━━━━━━━━━━━━━━━━━━\\nTotal: " << bb->operations.size() << " ops\\l";

        if (bb->hasConditionalBranch) file << "[BRANCH]\\l";
        if (bb->hasTableApply)        file << "[TABLE]\\l";
        if (bb->hasMemoryAccess)      file << "[MEMORY]\\l";
        if (bb->setsDropBit)          file << "[DROP]\\l";

        file << "\\nvDRMT: " << (bb->capability.canMapToVDRMT ? "✓" : "✗");
        file << " | vDPP: " << (bb->capability.canMapToVDPP ? "✓" : "✗") << "\\l";
        file << "\"";
      }

      file << "];\n";
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
          } else if (exit.condition == "forward") {
            file << ", color=\"blue\", style=\"bold\"";
          } else if (exit.condition == "drop") {
            file << ", color=\"darkred\", style=\"bold\"";
          }
          file << "];\n";
        }
      }
    }
    
    file << "\n  legend [shape=box, style=\"filled\", fillcolor=\"lightgray\", label=\"";
    file << "Legend:\\lYellow = Conditional\\lBlue = Table\\lGreen = Memory/Forward\\lSalmon = Drop terminal\\l\"];\n";
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