// ============================================================================
// File: lib/Pass/P4HIRToVDPP/P4HIRToVDPPPass.cpp
// P4HIR to vDPP Lowering Pass
// ============================================================================

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include "mlir/Pass/PassManager.h"

#include "Pass/P4HIRToVDPPPass.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Ops.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Types.h"
#include "p4mlir/Transforms/Passes.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"
#include "Dialect/vDPP/IR/vDPPOpInterfaces.h"

#include <fstream>
#include <set>
#include <vector>

using namespace mlir;
using namespace P4::P4MLIR::P4HIR;

namespace {

// ============================================================================
// Coarse-grained finalization
// ============================================================================
// Lift vDPP VFFA instance declarations to module scope and strip helper
// attrs. P4HIRToVDPP today emits vdpp.counter.count and vdpp.hash5tuple.apply
// without helper attrs (instance ops are produced upstream), so Step 2 is
// vacuous now. The symmetric structure is here so SHA/RegEx/Compress
// lifting drops in cleanly when LLVMIRToVDPP starts emitting them.
static void coarseGrainVDPP(ModuleOp mod) {
    DenseSet<StringAttr> declared;
    mod.walk([&](vdpp::VFFAInstanceOpInterface inst) {
        declared.insert(inst.getSymNameAttr());
    });
    // Step 2 (per-type lifting) intentionally empty: see comment above.
    mod.walk([&](vdpp::VFFAExecuteOpInterface use) {
        use.removeHelperAttrs();
    });
}

// ============================================================================
// Memory Object Information
// ============================================================================

struct MemoryObjectInfo {
  std::string name;
  int size;
  bool isReadOnly;
  bool isWriteHeavy;
  bool isShared;
  std::vector<int> accessingBlocks;
  int numReads;
  int numWrites;
  
  static std::vector<MemoryObjectInfo> loadFromJSON(StringRef jsonFile) {
    std::vector<MemoryObjectInfo> memObjects;
    
    auto fileOrErr = llvm::MemoryBuffer::getFile(jsonFile);
    if (!fileOrErr) return memObjects;
    
    llvm::StringRef jsonStr = fileOrErr.get()->getBuffer();
    auto jsonValue = llvm::json::parse(jsonStr);
    if (!jsonValue) return memObjects;
    
    auto *obj = jsonValue->getAsObject();
    if (!obj) return memObjects;
    
    auto *memArray = obj->getArray("memoryObjects");
    if (!memArray) return memObjects;
    
    for (auto &elem : *memArray) {
      auto *memObj = elem.getAsObject();
      if (!memObj) continue;
      
      MemoryObjectInfo info;
      
      if (auto name = memObj->getString("name")) info.name = name->str();
      if (auto size = memObj->getInteger("size")) info.size = *size;
      if (auto readOnly = memObj->getBoolean("isReadOnly")) info.isReadOnly = *readOnly;
      if (auto writeHeavy = memObj->getBoolean("isWriteHeavy")) info.isWriteHeavy = *writeHeavy;
      if (auto shared = memObj->getBoolean("isShared")) info.isShared = *shared;
      if (auto numReads = memObj->getInteger("numReads")) info.numReads = *numReads;
      if (auto numWrites = memObj->getInteger("numWrites")) info.numWrites = *numWrites;
      
      if (auto blocks = memObj->getArray("accessingBlocks")) {
        for (auto &blockElem : *blocks) {
          if (auto blockId = blockElem.getAsInteger()) {
            info.accessingBlocks.push_back(*blockId);
          }
        }
      }
      
      memObjects.push_back(info);
    }
    
    return memObjects;
  }
};

// ============================================================================
// Block Metadata
// ============================================================================

struct BlockMetadata {
  int blockId;
  std::string controlName;
  int operations;
  bool hasConditionalBranch;
  bool hasTableApply;
  bool hasMemoryAccess;
  bool canMapToVDRMT;
  bool isTerminal = false;
  std::vector<std::string> accessedMemoryObjects;

  static BlockMetadata loadFromJSON(StringRef jsonFile) {
    BlockMetadata meta;
    meta.blockId = -1;
    
    auto fileOrErr = llvm::MemoryBuffer::getFile(jsonFile);
    if (!fileOrErr) return meta;
    
    llvm::StringRef jsonStr = fileOrErr.get()->getBuffer();
    auto jsonValue = llvm::json::parse(jsonStr);
    if (!jsonValue) return meta;
    
    auto *obj = jsonValue->getAsObject();
    if (!obj) return meta;
    
    if (auto id = obj->getInteger("blockId")) meta.blockId = *id;
    if (auto name = obj->getString("controlName")) meta.controlName = name->str();
    if (auto ops = obj->getInteger("operations")) meta.operations = *ops;
    if (auto branch = obj->getBoolean("hasConditionalBranch")) meta.hasConditionalBranch = *branch;
    if (auto table = obj->getBoolean("hasTableApply")) meta.hasTableApply = *table;
    if (auto mem = obj->getBoolean("hasMemoryAccess")) meta.hasMemoryAccess = *mem;
    if (auto canMap = obj->getBoolean("canMapToVDRMT")) meta.canMapToVDRMT = *canMap;
    if (auto term = obj->getBoolean("isTerminal")) meta.isTerminal = *term;
    
    if (auto memObjs = obj->getArray("accessedMemoryObjects")) {
      for (auto &elem : *memObjs) {
        if (auto name = elem.getAsString()) {
          meta.accessedMemoryObjects.push_back(name->str());
        }
      }
    }
    
    return meta;
  }
};

struct BlockExitInfo {
  int exitId;
  int successor;
  std::string condition;
  
  static std::vector<BlockExitInfo> loadFromMetadata(StringRef metadataFile) {
    std::vector<BlockExitInfo> exits;
    
    auto fileOrErr = llvm::MemoryBuffer::getFile(metadataFile);
    if (!fileOrErr) return exits;
    
    auto jsonValue = llvm::json::parse(fileOrErr.get()->getBuffer());
    if (!jsonValue) return exits;
    
    auto *obj = jsonValue->getAsObject();
    if (!obj) return exits;
    
    auto *exitsArray = obj->getArray("exits");
    if (!exitsArray) return exits;
    
    for (auto &exitElem : *exitsArray) {
      auto *exitObj = exitElem.getAsObject();
      if (!exitObj) continue;
      
      BlockExitInfo info;
      if (auto id = exitObj->getInteger("exitId")) info.exitId = *id;
      if (auto succ = exitObj->getInteger("successor")) info.successor = *succ;
      if (auto cond = exitObj->getString("condition")) info.condition = cond->str();
      
      exits.push_back(info);
    }
    
    return exits;
  }
};

// ============================================================================
// P4HIR to vDPP Type Converter
// ============================================================================

class P4HIRToVDPPTypeConverter : public TypeConverter {
public:
  P4HIRToVDPPTypeConverter() {
    // Identity conversions for builtin types
    addConversion([](IntegerType type) { return type; });
    addConversion([](FloatType type) { return type; });
    addConversion([](IndexType type) { return type; });

    // Identity conversions for vDPP types
    addConversion([](vdpp::StructType type) { return type; });
    addConversion([](vdpp::PointerType type) { return type; });

    // P4HIR primitive type conversions
    addConversion([](P4::P4MLIR::P4HIR::BitsType type) -> Type {
      return IntegerType::get(type.getContext(), type.getWidth());
    });
    
    addConversion([](P4::P4MLIR::P4HIR::BoolType type) -> Type {
      return IntegerType::get(type.getContext(), 1);
    });
    
    addConversion([](P4::P4MLIR::P4HIR::ValidBitType type) -> Type {
      return IntegerType::get(type.getContext(), 1);
    });
    
    addConversion([](P4::P4MLIR::P4HIR::InfIntType type) -> Type {
      return IntegerType::get(type.getContext(), 32);
    });
    
    // P4HIR header → vDPP struct (LLVM-style)
    // Headers become named structs in vDPP
    addConversion([this](P4::P4MLIR::P4HIR::HeaderType p4hirHeader) -> Type {
      SmallVector<Type> fieldTypes;
      
      for (const auto &p4field : p4hirHeader.getElements()) {
        Type convertedType = convertType(p4field.type);
        if (!convertedType) return Type();
        fieldTypes.push_back(convertedType);
      }
      
      // Create named struct in vDPP (use explicit context for empty headers)
      return vdpp::StructType::get(
        p4hirHeader.getContext(),
        p4hirHeader.getName(),
        fieldTypes,
        false  // not packed
      );
    });
    
    // P4HIR struct → vDPP struct (LLVM-style)
    addConversion([this](P4::P4MLIR::P4HIR::StructType p4hirStruct) -> Type {
      SmallVector<Type> fieldTypes;
      
      for (const auto &p4field : p4hirStruct.getElements()) {
        Type convertedType = convertType(p4field.type);
        if (!convertedType) return Type();
        fieldTypes.push_back(convertedType);
      }
      
      // Create named struct in vDPP (use explicit context for empty structs)
      return vdpp::StructType::get(
        p4hirStruct.getContext(),
        p4hirStruct.getName(),
        fieldTypes,
        false  // not packed
      );
    });
    
    // P4HIR ref → vDPP pointer
    // References become pointers in vDPP (LLVM-style)
    addConversion([this](P4::P4MLIR::P4HIR::ReferenceType p4hirRef) -> Type {
      Type innerType = p4hirRef.getObjectType();
      Type convertedInner = convertType(innerType);
      if (!convertedInner) return Type();
      return vdpp::PointerType::get(convertedInner);
    });
    
    // P4HIR void
    addConversion([](P4::P4MLIR::P4HIR::VoidType type) -> Type {
      return type;
    });

    // P4HIR extern — keep as-is so adaptor construction doesn't fail when
    // a CallMethodOp's base operand has ExternType.  The InstantiateOp that
    // produces it is marked "always legal" and is erased post-conversion.
    addConversion([](P4::P4MLIR::P4HIR::ExternType type) -> Type {
      return type;
    });

    // Materializations: when the framework needs to bridge a type gap (e.g.
    // RegisterReadOp returns i32 but a not-yet-converted use expects
    // !p4hir.bit<32>), it inserts builtin.unrealized_conversion_cast.
    // These callbacks make that cast constructible.
    addSourceMaterialization(
        [](OpBuilder &builder, Type resultType, ValueRange inputs,
           Location loc) -> Value {
          return builder.create<mlir::UnrealizedConversionCastOp>(
              loc, resultType, inputs).getResult(0);
        });
    addTargetMaterialization(
        [](OpBuilder &builder, Type resultType, ValueRange inputs,
           Location loc) -> Value {
          return builder.create<mlir::UnrealizedConversionCastOp>(
              loc, resultType, inputs).getResult(0);
        });
  }
};

// ============================================================================
// Conversion Patterns
// ============================================================================

// Convert p4hir.const → vdpp.constant
class ConstOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::ConstOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::ConstOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::ConstOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type originalType = op.getType();
    Type convertedType = getTypeConverter()->convertType(originalType);
    if (!convertedType) return failure();
    
    auto valueAttr = op.getValueAttr();
    
    auto intType = convertedType.dyn_cast<IntegerType>();

    if (auto p4hirIntAttr = valueAttr.dyn_cast<P4::P4MLIR::P4HIR::IntAttr>()) {
      APInt value = p4hirIntAttr.getValue();
      if (intType) {
        if (value.getBitWidth() != intType.getWidth())
          value = value.zextOrTrunc(intType.getWidth());
        rewriter.replaceOpWithNewOp<vdpp::ConstantOp>(
            op, convertedType, IntegerAttr::get(intType, value));
        return success();
      }
    }

    // BoolAttr (#p4hir.bool<true/false>) → i1 integer constant.
    if (auto boolAttr = valueAttr.dyn_cast<P4::P4MLIR::P4HIR::BoolAttr>()) {
      if (intType) {
        rewriter.replaceOpWithNewOp<vdpp::ConstantOp>(
            op, convertedType,
            IntegerAttr::get(intType, boolAttr.getValue() ? 1 : 0));
        return success();
      }
    }

    // ValidityBitAttr (#p4hir<validity.bit valid/invalid>) → i1 integer constant.
    if (auto vbAttr = valueAttr.dyn_cast<P4::P4MLIR::P4HIR::ValidityBitAttr>()) {
      if (intType) {
        bool isValid =
            (vbAttr.getValue() == P4::P4MLIR::P4HIR::ValidityBit::Valid);
        rewriter.replaceOpWithNewOp<vdpp::ConstantOp>(
            op, convertedType,
            IntegerAttr::get(intType, isValid ? 1 : 0));
        return success();
      }
    }

    rewriter.replaceOpWithNewOp<vdpp::ConstantOp>(op, convertedType, valueAttr);
    return success();
  }
};

// Convert p4hir.binop → vdpp arithmetic/bitwise ops
class BinOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::BinOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::BinOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::BinOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    auto kind = static_cast<int>(op.getKind());
    
    // Map P4HIR BinOpKind to vDPP operations.
    // Enum: 1=Mul, 2=Div, 3=Mod, 4=Add, 5=Sub, 6=AddSat, 7=SubSat,
    //       8=Or, 9=Xor, 10=And (from P4HIR_Ops.td BinOpKind)
    switch (kind) {
      case 1: // Mul
        rewriter.replaceOpWithNewOp<vdpp::MulOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 2: // Div
        rewriter.replaceOpWithNewOp<vdpp::DivOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 3: // Mod
        return op.emitError("vDPP mod not yet supported");
      case 4: // Add
        rewriter.replaceOpWithNewOp<vdpp::AddOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 5: // Sub
        rewriter.replaceOpWithNewOp<vdpp::SubOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 6: // AddSat
        rewriter.replaceOpWithNewOp<vdpp::AddOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 7: // SubSat
        rewriter.replaceOpWithNewOp<vdpp::SubOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 8: // BOr
        rewriter.replaceOpWithNewOp<vdpp::OrOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 9: // BXor
        rewriter.replaceOpWithNewOp<vdpp::XorOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 10: // BAnd
        rewriter.replaceOpWithNewOp<vdpp::AndOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      default:
        return op.emitError("unsupported binary operation kind: ") << kind;
    }
    
    return success();
  }
};

// Convert p4hir.cmp → vdpp.icmp
class CmpOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::CmpOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::CmpOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::CmpOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    auto i1Type = rewriter.getI1Type();
    
    // Convert P4HIR CmpKind to vDPP ICmpPredicate
    auto p4Kind = static_cast<int>(op.getKind());
    auto vdppPredicate = static_cast<vdpp::ICmpPredicate>(p4Kind);
    
    rewriter.replaceOpWithNewOp<vdpp::ICmpOp>(
      op,
      i1Type,
      vdppPredicate,
      adaptor.getLhs(),
      adaptor.getRhs()
    );
    
    return success();
  }
};

// Convert p4hir.cast → vdpp.cast
class CastOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::CastOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::CastOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::CastOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    rewriter.replaceOpWithNewOp<vdpp::CastOp>(
      op,
      convertedType,
      adaptor.getSrc()
    );
    
    return success();
  }
};

// Convert p4hir.read → vdpp.load
class ReadOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::ReadOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::ReadOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::ReadOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    // P4HIR read from reference becomes vDPP load from pointer
    rewriter.replaceOpWithNewOp<vdpp::LoadOp>(
      op,
      convertedType,
      adaptor.getRef(),
      rewriter.getI64IntegerAttr(0)  // alignment = 0
    );
    
    return success();
  }
};

// Convert p4hir.call_method @Hash5Tuple::@apply → vdpp.hash5tuple.apply + vdpp.store
struct Hash5TupleCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty()) return failure();
        if (callee.getRootReference().getValue() != "Hash5Tuple") return failure();
        if (callee.getNestedReferences().back().getValue() != "apply") return failure();

        // arg_operands: [result_ref(out), src_addr, dst_addr, src_port, dst_port, proto]
        auto args = adaptor.getArgOperands();
        if (args.size() < 6) return failure();

        Value resultRef = args[0];
        Value srcAddr   = args[1];
        Value dstAddr   = args[2];
        Value srcPort   = args[3];
        Value dstPort   = args[4];
        Value proto     = args[5];

        // Determine instance name from the base operand (the Hash5Tuple instance).
        // Capture the InstantiateOp pointer before erasing the CallMethodOp.
        StringRef instanceName = "unknown_hash";
        P4::P4MLIR::P4HIR::InstantiateOp capturedInstOp;
        if (auto instOp = op.getBase().getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            capturedInstOp = instOp;
        }

        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));
        auto i32Ty  = rewriter.getI32Type();

        // Create vdpp.hash5tuple.apply — software hash, no nr_entries constraint.
        auto applyOp = rewriter.create<vdpp::Hash5TupleApplyOp>(
            op.getLoc(), i32Ty, symRef,
            srcAddr, dstAddr, srcPort, dstPort, proto);

        // Store the hash result into the out-parameter reference.
        rewriter.create<vdpp::StoreOp>(
            op.getLoc(),
            applyOp.getResult(),
            resultRef,
            rewriter.getI64IntegerAttr(0));

        rewriter.eraseOp(op);

        // The InstantiateOp that defined the base operand is now unused.
        // Erase it here so it doesn't remain as an illegal P4HIR op.
        if (capturedInstOp)
            rewriter.eraseOp(capturedInstOp);

        return success();
    }
};

// Convert p4hir.br → vdpp.br
// Produced by p4hir-flatten-cfg when lowering ScopeOp/IfOp/TernaryOp.
struct BrOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::BrOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::BrOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.replaceOpWithNewOp<vdpp::BranchOp>(
            op, op.getDest(), adaptor.getDestOperands());
        return success();
    }
};

// Convert p4hir.cond_br → vdpp.cond_br
struct CondBrOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::CondBrOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CondBrOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.replaceOpWithNewOp<vdpp::CondBranchOp>(
            op, adaptor.getCond(),
            op.getDestTrue(), op.getDestFalse(),
            adaptor.getDestOperandsTrue(),
            adaptor.getDestOperandsFalse());
        return success();
    }
};

// Erase p4hir.instantiate once all its uses have been consumed.
// Hash5TupleCallPattern handles the common case (erasing the InstantiateOp as
// part of the CallMethodOp conversion).  This pattern is a safety net for any
// instantiate that becomes use-empty through a different code path.
struct InstantiateOpErasePattern
    : OpConversionPattern<P4::P4MLIR::P4HIR::InstantiateOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::InstantiateOp op,
                                  OpAdaptor /*adaptor*/,
                                  ConversionPatternRewriter &rewriter) const override {
        if (!op->use_empty()) return failure();  // Not ready yet; will retry
        rewriter.eraseOp(op);
        return success();
    }
};

// ── Counter / Register / Meter / Table call patterns ────────────────────────
// Mirror the P4HIR→vDRMT patterns but emit vDPP ops.

// p4hir.call_method @Counter::@count → vdpp.counter.count
struct CounterCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty()) return failure();
        if (callee.getRootReference().getValue() != "Counter" ||
            callee.getNestedReferences().back().getValue() != "count")
            return failure();

        Value instanceVal = op.getBase();
        StringRef instanceName = "unknown_counter";
        int32_t size = 1024;
        if (auto instOp = instanceVal.getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            if (!instOp.getArgOperands().empty())
                if (auto cst = instOp.getArgOperands()[0]
                                     .getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>())
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        size = (int32_t)ia.getInt();
        }
        if (op.getArgOperands().empty()) return failure();
        Value idx = adaptor.getArgOperands()[0];
        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));
        auto countOp = rewriter.replaceOpWithNewOp<vdpp::CounterCountOp>(op, symRef, idx);
        countOp->setAttr("vdpp.counter_size", rewriter.getI32IntegerAttr(size));
        return success();
    }
};

// p4hir.call_method @Register::@read → vdpp.register.read
// p4hir.call_method @Register::@write → vdpp.register.write
struct RegisterCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty()) return failure();
        auto rootRef   = callee.getRootReference().getValue();
        auto methodRef = callee.getNestedReferences().back().getValue();
        if (rootRef != "Register") return failure();
        if (methodRef != "read" && methodRef != "write") return failure();

        Value instanceVal = op.getBase();
        StringRef instanceName = "unknown_register";
        int32_t size = 1024, elementWidth = 32;
        if (auto instOp = instanceVal.getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            if (!instOp.getArgOperands().empty())
                if (auto cst = instOp.getArgOperands()[0]
                                     .getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>())
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        size = (int32_t)ia.getInt();
            if (auto instType = instOp.getType())
                if (auto intTy = dyn_cast<IntegerType>(instType))
                    elementWidth = (int32_t)intTy.getWidth();
        }
        auto args   = adaptor.getArgOperands();
        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));

        if (methodRef == "read") {
            if (args.empty()) return failure();
            auto elemTy = rewriter.getIntegerType(elementWidth);
            llvm::errs() << "  [RegisterCallPattern] read: instance=" << instanceName
                         << " idx_type=" << args[0].getType()
                         << " elem_type=" << elemTy << "\n";
            auto readOp = rewriter.replaceOpWithNewOp<vdpp::RegisterReadOp>(
                op, elemTy, symRef, args[0]);
            readOp->setAttr("vdpp.register_size",          rewriter.getI32IntegerAttr(size));
            readOp->setAttr("vdpp.register_element_width", rewriter.getI32IntegerAttr(elementWidth));
        } else {
            if (args.size() < 2) return failure();
            auto writeOp = rewriter.create<vdpp::RegisterWriteOp>(
                op.getLoc(), symRef, args[0], args[1]);
            writeOp->setAttr("vdpp.register_size",          rewriter.getI32IntegerAttr(size));
            writeOp->setAttr("vdpp.register_element_width", rewriter.getI32IntegerAttr(elementWidth));
            rewriter.eraseOp(op);
        }
        return success();
    }
};

// Drop p4hir.table_apply — on the software (ARM/DPA) path, the table match is
// not performed by hardware; the block handler implements the action directly.
struct TableApplyDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::TableApplyOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::TableApplyOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Drop p4hir.call — action bodies are inlined by the partitioner.
struct CallOpDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Drop leftover p4hir.call_method that wasn't matched by a specific extern
// pattern (Counter/Register/Hash5Tuple). Lower priority than those patterns.
struct CallMethodDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Convert p4hir.variable → vdpp.alloca
// p4hir.variable defines a scope-local mutable slot; its result type is
// !p4hir.ref<T>, which the type converter maps to !vdpp.ptr<T>.
struct VariableOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::VariableOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::VariableOp op,
                                  OpAdaptor /*adaptor*/,
                                  ConversionPatternRewriter &rewriter) const override {
        Type convertedRefType = getTypeConverter()->convertType(op.getRef().getType());
        if (!convertedRefType) return failure();
        auto ptrType = mlir::dyn_cast<vdpp::PointerType>(convertedRefType);
        if (!ptrType) return failure();
        rewriter.replaceOpWithNewOp<vdpp::AllocaOp>(op, ptrType.getElementType());
        return success();
    }
};

// Convert p4hir.scope (void) → inline the body into the parent block.
// Scopes that yield a value are not supported here (failure() causes fallback).
struct ScopeOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::ScopeOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::ScopeOp op,
                                  OpAdaptor /*adaptor*/,
                                  ConversionPatternRewriter &rewriter) const override {
        // Only handle void scopes (no result).
        if (op->getNumResults() > 0) return failure();

        Location loc = op.getLoc();
        Block *currentBlock = op->getBlock();
        Region *parentRegion = currentBlock->getParent();

        // Split so that ops after the scope land in continueBlock.
        Block *continueBlock = rewriter.splitBlock(currentBlock, ++Block::iterator(op));

        // Inline the scope's single-block region before continueBlock.
        Region &body = op.getScopeRegion();
        rewriter.inlineRegionBefore(body, *parentRegion, Region::iterator(continueBlock));

        // The inlined block is now immediately before continueBlock.
        Block *inlinedBlock = &*std::prev(Region::iterator(continueBlock));

        // Move inlined ops to the end of currentBlock.
        currentBlock->getOperations().splice(currentBlock->end(),
                                             inlinedBlock->getOperations());

        // The implicit/explicit yield is now the last op in currentBlock; erase it.
        if (!currentBlock->empty()) {
            if (isa<P4::P4MLIR::P4HIR::YieldOp>(currentBlock->back()))
                rewriter.eraseOp(&currentBlock->back());
        }

        // Fall through from currentBlock to continueBlock.
        rewriter.setInsertionPointToEnd(currentBlock);
        rewriter.create<vdpp::BranchOp>(loc, continueBlock);

        rewriter.eraseBlock(inlinedBlock);
        rewriter.eraseOp(op);
        return success();
    }
};

// Convert p4hir.ternary → CFG with block argument at merge point.
//
//   %result = p4hir.ternary(%cond, true { ... yield %t }, false { ... yield %f })
//
// lowers to:
//
//   vdpp.cond_br %cond, ^trueBlock, ^falseBlock
//   ^trueBlock:  ... ; vdpp.br ^mergeBlock(%t)
//   ^falseBlock: ... ; vdpp.br ^mergeBlock(%f)
//   ^mergeBlock(%result : T):
//   ... (original ops after ternary, using %result)
struct TernaryOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::TernaryOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::TernaryOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        Location loc = op.getLoc();

        // The ternary must have exactly one result.
        if (!op.getResult()) return failure();
        Type resultType = getTypeConverter()->convertType(op.getResult().getType());
        if (!resultType) return failure();

        Block *currentBlock = op->getBlock();
        Region *parentRegion = currentBlock->getParent();

        // Split: ops after the ternary go into mergeBlock, which receives the
        // ternary result as a block argument.
        Block *mergeBlock = rewriter.splitBlock(currentBlock, ++Block::iterator(op));
        mergeBlock->addArgument(resultType, loc);

        // New true/false blocks.
        Block *trueBlock  = new Block();
        Block *falseBlock = new Block();
        parentRegion->getBlocks().insert(Region::iterator(mergeBlock), trueBlock);
        parentRegion->getBlocks().insert(Region::iterator(mergeBlock), falseBlock);

        // Terminate currentBlock with a conditional branch.
        rewriter.setInsertionPoint(currentBlock, currentBlock->end());
        rewriter.create<vdpp::CondBranchOp>(loc, adaptor.getCond(), trueBlock, falseBlock);
        rewriter.eraseOp(op);

        // Replace all uses of the ternary result with the merge-block argument.
        op.getResult().replaceAllUsesWith(mergeBlock->getArgument(0));

        // Helper: inline one region, move ops to destBlock, replace yield with br.
        auto inlineRegion = [&](Region &region, Block *destBlock) {
            if (region.empty()) {
                rewriter.setInsertionPointToEnd(destBlock);
                rewriter.create<vdpp::BranchOp>(loc, mergeBlock,
                                                ValueRange{mergeBlock->getArgument(0)});
                return;
            }
            rewriter.inlineRegionBefore(region, *parentRegion,
                                        Region::iterator(mergeBlock));
            Block *inlined = &*std::prev(Region::iterator(mergeBlock));
            destBlock->getOperations().splice(destBlock->begin(),
                                              inlined->getOperations());
            // Replace the terminating yield with a branch to mergeBlock.
            if (!destBlock->empty()) {
                if (auto yieldOp =
                        dyn_cast<P4::P4MLIR::P4HIR::YieldOp>(destBlock->back())) {
                    Value yieldedVal = yieldOp.getArgs().front();
                    rewriter.setInsertionPoint(yieldOp);
                    rewriter.replaceOpWithNewOp<vdpp::BranchOp>(
                        yieldOp, mergeBlock, ValueRange{yieldedVal});
                }
            }
            rewriter.eraseBlock(inlined);
        };

        inlineRegion(op.getTrueRegion(),  trueBlock);
        inlineRegion(op.getFalseRegion(), falseBlock);

        return success();
    }
};

// Convert p4hir.assign → vdpp.store
class AssignOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::AssignOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::AssignOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::AssignOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    // P4HIR assign to reference becomes vDPP store to pointer
    rewriter.replaceOpWithNewOp<vdpp::StoreOp>(
      op,
      adaptor.getValue(),
      adaptor.getRef(),
      rewriter.getI64IntegerAttr(0)  // alignment = 0
    );
    
    return success();
  }
};

// Convert p4hir.struct_extract → extract field from struct value
class StructExtractOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::StructExtractOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedResultType = getTypeConverter()->convertType(op.getType());
    if (!convertedResultType) return failure();
    
    auto input = adaptor.getInput();
    Type inputType = input.getType();
    auto fieldIndex = op.getFieldIndex();
    
    // Strategy: Store struct to stack, GEP to field, load field
    // This works for any struct value
    
    // 1. Get the struct type
    auto structType = inputType.dyn_cast<vdpp::StructType>();
    if (!structType) {
      // Input might be a pointer - try to get element type
      if (auto ptrType = inputType.dyn_cast<vdpp::PointerType>()) {
        structType = ptrType.getElementType().dyn_cast<vdpp::StructType>();
      }
    }
    
    if (!structType) {
      return op.emitError("struct_extract input must be a struct type or pointer to struct");
    }
    
    // Check if input is already a pointer
    if (auto ptrType = inputType.dyn_cast<vdpp::PointerType>()) {
      // Input is a pointer to struct, we can GEP directly
      auto i32Type = rewriter.getI32Type();
      auto zero = rewriter.create<vdpp::ConstantOp>(
        op.getLoc(),
        i32Type,
        rewriter.getI32IntegerAttr(0)
      );
      auto fieldIndexVal = rewriter.create<vdpp::ConstantOp>(
        op.getLoc(),
        i32Type,
        rewriter.getI32IntegerAttr(fieldIndex)
      );
      
      SmallVector<Value> indices = {zero, fieldIndexVal};
      
      // GEP to get field pointer
      auto fieldPtrType = vdpp::PointerType::get(convertedResultType);
      auto fieldPtr = rewriter.create<vdpp::GetElementPtrOp>(
        op.getLoc(),
        fieldPtrType,
        input,
        indices
      );
      
      // Load the field value
      rewriter.replaceOpWithNewOp<vdpp::LoadOp>(
        op,
        convertedResultType,
        fieldPtr,
        rewriter.getI64IntegerAttr(0)  // alignment
      );
      
      return success();
    }
    
    // Input is a struct value (not pointer)
    // Strategy: alloca, store, GEP, load
    
    // 2. Allocate space for the struct on stack
    auto structPtrType = vdpp::PointerType::get(structType);
    auto allocaOp = rewriter.create<vdpp::AllocaOp>(
      op.getLoc(),
      structPtrType,
      Value(),  // no array size
      rewriter.getI64IntegerAttr(8)  // alignment = 8
    );
    
    // 3. Store the struct value to the allocated space
    rewriter.create<vdpp::StoreOp>(
      op.getLoc(),
      input,
      allocaOp.getResult(),
      rewriter.getI64IntegerAttr(0)  // alignment
    );
    
    // 4. GEP to get pointer to the field
    auto i32Type = rewriter.getI32Type();
    auto zero = rewriter.create<vdpp::ConstantOp>(
      op.getLoc(),
      i32Type,
      rewriter.getI32IntegerAttr(0)
    );
    auto fieldIndexVal = rewriter.create<vdpp::ConstantOp>(
      op.getLoc(),
      i32Type,
      rewriter.getI32IntegerAttr(fieldIndex)
    );
    
    SmallVector<Value> indices = {zero, fieldIndexVal};
    
    auto fieldPtrType = vdpp::PointerType::get(convertedResultType);
    auto fieldPtr = rewriter.create<vdpp::GetElementPtrOp>(
      op.getLoc(),
      fieldPtrType,
      allocaOp.getResult(),
      indices
    );
    
    // 5. Load the field value
    rewriter.replaceOpWithNewOp<vdpp::LoadOp>(
      op,
      convertedResultType,
      fieldPtr,
      rewriter.getI64IntegerAttr(0)  // alignment
    );
    
    return success();
  }
};

// Convert p4hir.struct_extract_ref → vdpp.getelementptr
class StructExtractRefOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractRefOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractRefOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::StructExtractRefOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedResultType = getTypeConverter()->convertType(op.getType());
    if (!convertedResultType) return failure();
    
    // P4HIR: struct_extract_ref %ptr[index] : ref<struct> -> ref<field>
    // vDPP:  getelementptr %ptr[0, index] : ptr<struct> -> ptr<field>
    
    auto fieldIndex = op.getFieldIndex();
    
    // GEP indices: [0, fieldIndex]
    // First 0 dereferences the pointer, second is the field index
    auto i32Type = rewriter.getI32Type();
    auto zero = rewriter.create<vdpp::ConstantOp>(
      op.getLoc(),
      i32Type,
      rewriter.getI32IntegerAttr(0)
    );
    auto fieldIndexVal = rewriter.create<vdpp::ConstantOp>(
      op.getLoc(),
      i32Type,
      rewriter.getI32IntegerAttr(fieldIndex)
    );
    
    SmallVector<Value> indices = {zero, fieldIndexVal};
    
    rewriter.replaceOpWithNewOp<vdpp::GetElementPtrOp>(
      op,
      convertedResultType,
      adaptor.getInput(),
      indices
    );
    
    return success();
  }
};

// Convert p4hir.if → vdpp CFG with basic blocks + successor annotations
class IfOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::IfOp> {
public:
  IfOpConversion(TypeConverter &typeConverter, MLIRContext *context,
                 std::vector<BlockExitInfo> exitInfo)
    : OpConversionPattern<P4::P4MLIR::P4HIR::IfOp>(typeConverter, context),
      exitInfo(std::move(exitInfo)) {}
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::IfOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Location loc = op.getLoc();
    Block *currentBlock = op->getBlock();
    
    // Split the block after the if operation
    Block *continueBlock = rewriter.splitBlock(currentBlock, ++Block::iterator(op));
    
    // Move insertion point back
    rewriter.setInsertionPoint(currentBlock, currentBlock->end());
    
    // Create then and else blocks
    Region *parentRegion = currentBlock->getParent();
    Block *thenBlock = new Block();
    Block *elseBlock = nullptr;
    
    Region &elseRegion = op.getElseRegion();
    bool hasElse = !elseRegion.empty();
    
    if (hasElse) {
      elseBlock = new Block();
    } else {
      elseBlock = continueBlock;
    }
    
    // Insert blocks
    parentRegion->getBlocks().insert(Region::iterator(continueBlock), thenBlock);
    if (hasElse) {
      parentRegion->getBlocks().insert(Region::iterator(continueBlock), elseBlock);
    }
    
    // Create conditional branch
    rewriter.create<vdpp::CondBranchOp>(
      loc,
      adaptor.getCondition(),
      thenBlock,
      elseBlock
    );
    
    // Erase the if operation
    rewriter.eraseOp(op);
    
    // Process then region
    Region &thenRegion = op.getThenRegion();
    if (!thenRegion.empty()) {
      rewriter.inlineRegionBefore(thenRegion, *parentRegion, Region::iterator(continueBlock));
      
      Block *inlinedThenBlock = &*std::prev(Region::iterator(continueBlock));
      
      thenBlock->getOperations().splice(
        thenBlock->begin(),
        inlinedThenBlock->getOperations()
      );
      
      // Fix terminator with successor annotation
      if (!thenBlock->empty()) {
        Operation &lastOp = thenBlock->back();
        if (auto yieldOp = dyn_cast<P4::P4MLIR::P4HIR::YieldOp>(&lastOp)) {
          rewriter.setInsertionPoint(yieldOp);
          
          // Get successor from exit info (exit 0 = then branch)
          int successor = -1;  // Default: end of pipeline
          if (!exitInfo.empty() && exitInfo.size() > 0) {
            successor = exitInfo[0].successor;
          }
          
          // Create return with successor attribute
          rewriter.replaceOpWithNewOp<vdpp::ReturnOp>(
            yieldOp,
            ValueRange{},
            successor
          );
        } else if (!lastOp.hasTrait<OpTrait::IsTerminator>()) {
          rewriter.setInsertionPointToEnd(thenBlock);
          rewriter.create<vdpp::BranchOp>(loc, continueBlock);
        }
      }
      
      rewriter.eraseBlock(inlinedThenBlock);
    } else {
      rewriter.setInsertionPointToEnd(thenBlock);
      rewriter.create<vdpp::BranchOp>(loc, continueBlock);
    }
    
    // Process else region
    if (hasElse && !elseRegion.empty()) {
      rewriter.inlineRegionBefore(elseRegion, *parentRegion, Region::iterator(continueBlock));
      
      Block *inlinedElseBlock = &*std::prev(Region::iterator(continueBlock));
      
      elseBlock->getOperations().splice(
        elseBlock->begin(),
        inlinedElseBlock->getOperations()
      );
      
      // Fix terminator with successor annotation
      if (!elseBlock->empty()) {
        Operation &lastOp = elseBlock->back();
        if (auto yieldOp = dyn_cast<P4::P4MLIR::P4HIR::YieldOp>(&lastOp)) {
          rewriter.setInsertionPoint(yieldOp);
          
          // Get successor from exit info (exit 1 = else branch)
          int successor = -1;
          if (exitInfo.size() > 1) {
            successor = exitInfo[1].successor;
          }
          
          // Create return with successor attribute
          rewriter.replaceOpWithNewOp<vdpp::ReturnOp>(
            yieldOp,
            ValueRange{},
            successor
          );
        } else if (!lastOp.hasTrait<OpTrait::IsTerminator>()) {
          rewriter.setInsertionPointToEnd(elseBlock);
          rewriter.create<vdpp::BranchOp>(loc, continueBlock);
        }
      }
      
      rewriter.eraseBlock(inlinedElseBlock);
    }
    
    return success();
  }

private:
  std::vector<BlockExitInfo> exitInfo;
};

// Convert func.return → vdpp.return with successor annotation
class ReturnOpConversion : public OpConversionPattern<func::ReturnOp> {
public:
  ReturnOpConversion(TypeConverter &typeConverter, MLIRContext *context,
                     std::vector<BlockExitInfo> exitInfo)
    : OpConversionPattern<func::ReturnOp>(typeConverter, context),
      exitInfo(std::move(exitInfo)) {}
  
  LogicalResult matchAndRewrite(
      func::ReturnOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    // Get the successor for this return
    // For simple blocks (no if), use exit 0
    int successor = -1;  // Default: end of pipeline
    if (!exitInfo.empty()) {
      successor = exitInfo[0].successor;
    }
    
    // func.return → vdpp.return with successor
    rewriter.replaceOpWithNewOp<vdpp::ReturnOp>(
      op, 
      adaptor.getOperands(),
      successor
    );
    
    return success();
  }

private:
  std::vector<BlockExitInfo> exitInfo;
};

// ============================================================================
// vDPP Block Generator
// ============================================================================

class VDPPBlockGenerator {
public:
  VDPPBlockGenerator(MLIRContext *context, StringRef inputDir)
    : context(context), inputDir(inputDir.str()), typeConverter() {}
  
  LogicalResult generate() {
    std::string memoryFile = inputDir + "/memory_objects.json";
    std::vector<MemoryObjectInfo> memObjects;
    
    if (llvm::sys::fs::exists(memoryFile)) {
      memObjects = MemoryObjectInfo::loadFromJSON(memoryFile);
      llvm::outs() << "  Loaded " << memObjects.size() << " memory objects\n";
    } else {
      llvm::outs() << "  No memory objects (stateless)\n";
    }
    
    llvm::outs() << "\n";
    
    int successCount = 0;
    int skipCount = 0;
    
    // Generate vDPP for ALL blocks (try them all, let conversion decide)
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator dir(inputDir, ec), end;
         dir != end && !ec; dir.increment(ec)) {
      
      auto path = dir->path();
      if (!llvm::sys::fs::is_directory(path)) continue;
      
      std::string dirName = llvm::sys::path::filename(path).str();
      
      if (dirName.find("block") == 0) {
        std::string metadataFile = path + "/metadata.json";
        auto meta = BlockMetadata::loadFromJSON(metadataFile);
        
        if (meta.blockId < 0) {
          llvm::errs() << "    ✗ Failed to load metadata for " << dirName << "\n";
          continue;
        }

        if (meta.isTerminal) continue;

        std::string p4hirFile = path + "/p4hir.mlir";
        std::string vdppFile = path + "/vdpp.mlir";
        
        // Try to generate vDPP for this block
        if (succeeded(generateBlockVDPP(p4hirFile, vdppFile, meta, memObjects))) {
          successCount++;
          llvm::outs() << "    ✓ Generated " << dirName << "/vdpp.mlir\n";
        } else {
          // Silent skip - this is normal for vDRMT-compatible blocks
          skipCount++;
        }
      }
    }
    
    llvm::outs() << "\n  Summary:\n";
    llvm::outs() << "    ✓ Generated: " << successCount << " blocks\n";
    if (skipCount > 0) {
      llvm::outs() << "    ⊘ Skipped:   " << skipCount << " blocks\n";
    }
    
    return success();
  }

private:
  MLIRContext *context;
  std::string inputDir;
  P4HIRToVDPPTypeConverter typeConverter;
  
  LogicalResult generateBlockVDPP(StringRef p4hirFile,
                                  StringRef vdppFile,
                                  BlockMetadata &meta,
                                  std::vector<MemoryObjectInfo> &memObjects) {
    // Parse P4HIR module
    auto p4hirModule = parseSourceFile<ModuleOp>(p4hirFile, context);
    if (!p4hirModule) return failure();

    // Flatten the P4HIR CFG: this converts ScopeOp, TernaryOp, IfOp, etc.
    // into flat multi-block CFG using p4hir.br / p4hir.cond_br terminators.
    // Running this BEFORE applyPartialConversion avoids nested-region ordering
    // issues where the conversion framework visits ops before their enclosing
    // structural op (ternary / scope) is lowered.
    {
      mlir::PassManager pm(context);
      pm.addPass(P4::P4MLIR::createFlattenCFGPass());
      if (failed(pm.run(*p4hirModule))) return failure();
    }

    // Find the function
    func::FuncOp p4hirFunc;
    p4hirModule->walk([&](func::FuncOp func) {
      p4hirFunc = func;
      return WalkResult::interrupt();
    });

    if (!p4hirFunc) return failure();

    // Load exit information from metadata
    std::string metadataFile = llvm::sys::path::parent_path(p4hirFile).str() + "/metadata.json";
    auto exitInfo = BlockExitInfo::loadFromMetadata(metadataFile);
    
    // Create vDPP module
    auto vdppModule = ModuleOp::create(UnknownLoc::get(context));
    OpBuilder builder(context);
    builder.setInsertionPointToStart(vdppModule.getBody());
    
    // Convert function signature types
    SmallVector<Type> convertedArgTypes;
    for (auto argType : p4hirFunc.getFunctionType().getInputs()) {
      Type converted = typeConverter.convertType(argType);
      if (!converted) {
        vdppModule.erase();
        return failure();
      }
      convertedArgTypes.push_back(converted);
    }
    
    SmallVector<Type> convertedResultTypes;
    for (auto resultType : p4hirFunc.getFunctionType().getResults()) {
      Type converted = typeConverter.convertType(resultType);
      if (!converted) {
        vdppModule.erase();
        return failure();
      }
      convertedResultTypes.push_back(converted);
    }
    
    auto vdppFuncType = builder.getFunctionType(convertedArgTypes, convertedResultTypes);
    
    // Create vDPP function
    auto vdppFunc = builder.create<func::FuncOp>(
      p4hirFunc.getLoc(),
      "vdpp_block" + std::to_string(meta.blockId),
      vdppFuncType
    );
    
    // Add vDPP metadata including successors
    vdppFunc->setAttr("vdpp.block_id", builder.getI32IntegerAttr(meta.blockId));
    vdppFunc->setAttr("vdpp.control_name", builder.getStringAttr(meta.controlName));
    
    // Add successor information as attribute
    if (!exitInfo.empty()) {
      SmallVector<int32_t> successors;
      for (auto &exit : exitInfo) {
        successors.push_back(exit.successor);
      }
      vdppFunc->setAttr("vdpp.successors", 
        builder.getI32ArrayAttr(successors));
    }
    
    // Set up conversion target
    ConversionTarget target(*context);
    target.addLegalDialect<vdpp::vDPPDialect>();
    target.addLegalDialect<func::FuncDialect>();
    target.addIllegalDialect<P4::P4MLIR::P4HIR::P4HIRDialect>();

    // p4hir.instantiate is treated as dynamically legal so the framework does
    // not fail when it encounters it before the scope containing its user has
    // been inlined.  Hash5TupleCallPattern erases InstantiateOp together with
    // the CallMethodOp; any that remain after applyPartialConversion are cleaned
    // up in the post-processing walk below.
    target.addDynamicallyLegalOp<P4::P4MLIR::P4HIR::InstantiateOp>(
        [](P4::P4MLIR::P4HIR::InstantiateOp op) { return true; });

    // Mark func.return as illegal so it gets converted to vdpp.return
    target.addIllegalOp<func::ReturnOp>();

    // Allow unrealized_conversion_cast to persist — they bridge type gaps
    // between P4HIR types (!p4hir.bit<N>) and builtin types (iN) when
    // conversion patterns fire in different order. A post-conversion
    // cleanup pass can reconcile or erase them.
    target.addLegalOp<mlir::UnrealizedConversionCastOp>();
    
    // Helper: build the full pattern set.  Called twice — once for the main
    // conversion, and once for a follow-up pass that handles ops in blocks
    // created by TernaryOpConversion (those blocks are not visited by the
    // first pass because they are inserted via raw block::splice).
    auto buildPatterns = [&]() {
      RewritePatternSet p(context);
      p.add<
        ConstOpConversion,
        BinOpConversion,
        CmpOpConversion,
        CastOpConversion,
        ReadOpConversion,
        AssignOpConversion,
        StructExtractOpConversion,
        StructExtractRefOpConversion,
        TableApplyDropPattern,
        CallOpDropPattern,
        InstantiateOpErasePattern,
        VariableOpConversion,
        ScopeOpConversion,
        TernaryOpConversion,
        BrOpConversion,
        CondBrOpConversion
      >(typeConverter, context);
      // Extern call patterns: higher benefit so they match before CallMethodDropPattern.
      p.add<
        Hash5TupleCallPattern,
        CounterCallPattern,
        RegisterCallPattern
      >(typeConverter, context, /*benefit=*/PatternBenefit(2));
      p.add<CallMethodDropPattern>(typeConverter, context, /*benefit=*/PatternBenefit(1));
      p.add<IfOpConversion>(typeConverter, context, exitInfo);
      p.add<ReturnOpConversion>(typeConverter, context, exitInfo);
      return p;
    };

    // Clone the entire p4hir function body (all blocks) into vdppFunc.
    // After p4hir-flatten-cfg, the function may have multiple blocks
    // (for ternary/if CFG edges).  We must clone ALL blocks so that block
    // successor references remain valid in the vdpp module.
    Block *vdppBody = vdppFunc.addEntryBlock();
    IRMapping mapping;

    Region &p4hirRegion = p4hirFunc.getBody();
    Region &vdppRegion = vdppFunc.getBody();

    // Map entry block and its function arguments.
    Block *p4hirEntryBlock = &p4hirRegion.front();
    mapping.map(p4hirEntryBlock, vdppBody);
    for (unsigned i = 0; i < p4hirFunc.getNumArguments(); ++i)
      mapping.map(p4hirEntryBlock->getArgument(i), vdppBody->getArgument(i));

    // Create new (empty) blocks for all non-entry blocks, converting
    // any P4HIR-typed block arguments to their vdpp equivalents.
    for (auto &block : llvm::drop_begin(p4hirRegion)) {
      Block *newBlock = new Block();
      vdppRegion.push_back(newBlock);
      mapping.map(&block, newBlock);
      for (auto arg : block.getArguments()) {
        Type converted = typeConverter.convertType(arg.getType());
        Type newArgType = converted ? converted : arg.getType();
        mapping.map(arg, newBlock->addArgument(newArgType, arg.getLoc()));
      }
    }

    // Clone all ops in all blocks using the full mapping so that block
    // successors and value operands are remapped correctly.
    OpBuilder bodyBuilder(context);
    for (auto &block : p4hirRegion) {
      Block *targetBlock = mapping.lookupOrNull(&block);
      if (!targetBlock) continue;
      bodyBuilder.setInsertionPointToEnd(targetBlock);
      for (auto &op : block.getOperations())
        bodyBuilder.clone(op, mapping);
    }

    // First conversion pass.
    if (failed(applyPartialConversion(vdppFunc, target, buildPatterns()))) {
      vdppModule.erase();
      return failure();
    }

    // Second pass: TernaryOpConversion and ScopeOpConversion move ops into
    // freshly created blocks via raw splice.  Those blocks are not in the
    // first pass's traversal, so any remaining P4HIR ops inside them need a
    // second sweep.
    if (failed(applyPartialConversion(vdppFunc, target, buildPatterns()))) {
      vdppModule.erase();
      return failure();
    }

    // Clean up any p4hir.instantiate ops that became use-empty after conversion
    // (Hash5TupleCallPattern normally erases them, but use_empty is the guard).
    vdppFunc.walk([&](P4::P4MLIR::P4HIR::InstantiateOp instOp) {
      if (instOp->use_empty())
        instOp->erase();
    });

    // Declare VFFA instances used in this block at module scope.
    // Each block module is self-contained, so instance declarations must
    // be present in the same vdpp.mlir file as the ops that reference them.
    {
      std::set<std::string> declaredInstances;
      OpBuilder instBuilder(vdppModule.getBody(),
                            vdppModule.getBody()->begin());

      vdppFunc.walk([&](vdpp::Hash5TupleApplyOp applyOp) {
        std::string instName = applyOp.getInstance().str();
        if (declaredInstances.insert(instName).second)
          instBuilder.create<vdpp::Hash5TupleInstanceOp>(
              vdppFunc.getLoc(),
              instBuilder.getStringAttr(StringRef(instName)));
      });

      vdppFunc.walk([&](vdpp::CounterCountOp countOp) {
        std::string instName = countOp.getInstance().str();
        int32_t size = 1024;
        if (auto attr = countOp->getAttrOfType<IntegerAttr>("vdpp.counter_size"))
          size = (int32_t)attr.getInt();
        if (declaredInstances.insert(instName).second)
          instBuilder.create<vdpp::CounterInstanceOp>(
              vdppFunc.getLoc(),
              instBuilder.getStringAttr(StringRef(instName)),
              instBuilder.getI32IntegerAttr(size));
      });

      // Register instances: collect from both read and write ops.
      auto declareRegister = [&](StringRef instName, int32_t size, int32_t elemWidth) {
        if (declaredInstances.insert(instName.str()).second)
          instBuilder.create<vdpp::RegisterInstanceOp>(
              vdppFunc.getLoc(),
              instBuilder.getStringAttr(instName),
              instBuilder.getI32IntegerAttr(size),
              instBuilder.getI32IntegerAttr(elemWidth));
      };
      vdppFunc.walk([&](vdpp::RegisterReadOp readOp) {
        int32_t size = 1024, elemWidth = 32;
        if (auto a = readOp->getAttrOfType<IntegerAttr>("vdpp.register_size"))
          size = (int32_t)a.getInt();
        if (auto a = readOp->getAttrOfType<IntegerAttr>("vdpp.register_element_width"))
          elemWidth = (int32_t)a.getInt();
        declareRegister(readOp.getInstance(), size, elemWidth);
      });
      vdppFunc.walk([&](vdpp::RegisterWriteOp writeOp) {
        int32_t size = 1024, elemWidth = 32;
        if (auto a = writeOp->getAttrOfType<IntegerAttr>("vdpp.register_size"))
          size = (int32_t)a.getInt();
        if (auto a = writeOp->getAttrOfType<IntegerAttr>("vdpp.register_element_width"))
          elemWidth = (int32_t)a.getInt();
        declareRegister(writeOp.getInstance(), size, elemWidth);
      });
    }

    // Post-process to add successor to final return if it doesn't have one
    if (!exitInfo.empty()) {
      vdppFunc.walk([&](vdpp::ReturnOp returnOp) {
        // Check if this return already has a successor attribute
        if (!returnOp->hasAttr("successor")) {
          // Get the default exit (should be exit 0 for non-branching blocks)
          int successor = exitInfo[0].successor;
          returnOp->setAttr("successor", builder.getI32IntegerAttr(successor));
        }
      });
    }
    
    // Coarse-grained finalization (lift vDPP VFFA decls + strip helper attrs).
    coarseGrainVDPP(vdppModule);

    // Write to file
    std::error_code ec;
    llvm::raw_fd_ostream file(vdppFile, ec);
    if (ec) {
      vdppModule.erase();
      return failure();
    }
    
    OpPrintingFlags flags;
    flags.printGenericOpForm(false);
    flags.enableDebugInfo(false, false);
    
    vdppModule.print(file, flags);
    vdppModule.erase();
    
    return success();
  }
};

// ============================================================================
// P4HIR to vDPP Lowering Pass
// ============================================================================

struct P4HIRToVDPPPass 
  : public PassWrapper<P4HIRToVDPPPass, OperationPass<ModuleOp>> {
  
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(P4HIRToVDPPPass)
  
  P4HIRToVDPPPass() = default;
  P4HIRToVDPPPass(const P4HIRToVDPPPass &other) : PassWrapper(other) {}
  
  std::string inputDir = "vdsa_output";
  
  StringRef getArgument() const final { return "p4hir-to-vdpp"; }
  
  StringRef getDescription() const final {
    return "Lower partitioned P4HIR to vDPP (generates vdpp.mlir per block)";
  }
  
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<vdpp::vDPPDialect>();
    registry.insert<func::FuncDialect>();
    registry.insert<LLVM::LLVMDialect>();
  }
  
  void runOnOperation() override {
    auto *context = &getContext();
    
    llvm::outs() << "\n";
    llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    llvm::outs() << "🎯 P4HIR to vDPP Lowering Pass\n";
    llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    llvm::outs() << "\n";
    
    llvm::SmallString<256> cwd;
    llvm::sys::fs::current_path(cwd);
    llvm::outs() << "Current directory: " << cwd << "\n";
    llvm::outs() << "Looking for: " << inputDir << "\n\n";
    
    if (!llvm::sys::fs::exists(inputDir)) {
      llvm::errs() << "❌ Input directory not found: " << inputDir << "\n";
      signalPassFailure();
      return;
    }
    
    llvm::outs() << "✓ Found partition directory\n\n";
    
    VDPPBlockGenerator generator(context, inputDir);
    
    if (failed(generator.generate())) {
      llvm::errs() << "Failed to generate vDPP files\n";
      signalPassFailure();
      return;
    }
    
    llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    llvm::outs() << "✅ vDPP lowering complete!\n";
    llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
  }
};

} // anonymous namespace

namespace mlir {

std::unique_ptr<Pass> createP4HIRToVDPPPass() {
  return std::make_unique<P4HIRToVDPPPass>();
}

void registerP4HIRToVDPPPass() {
  PassRegistration<P4HIRToVDPPPass>();
}

} // namespace mlir