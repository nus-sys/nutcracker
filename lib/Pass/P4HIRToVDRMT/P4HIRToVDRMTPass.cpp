// ============================================================================
// File: lib/Pass/P4HIRToVDRMT/P4HIRToVDRMT.cpp
// P4HIR to vDRMT Lowering Pass - WITH SUCCESSOR TRACKING
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

#include "Pass/P4HIRToVDRMTPass.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Ops.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Types.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Attrs.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

#include <fstream>
#include <vector>

using namespace mlir;
using namespace P4::P4MLIR::P4HIR;

namespace {

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

// ============================================================================
// Block Exit Information (for successor tracking)
// ============================================================================

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
// P4HIR to vDRMT Type Converter
// ============================================================================

class P4HIRToVDRMTTypeConverter : public TypeConverter {
public:
  P4HIRToVDRMTTypeConverter() {
    // Identity conversions for builtin types
    addConversion([](IntegerType type) { return type; });
    addConversion([](FloatType type) { return type; });
    addConversion([](IndexType type) { return type; });

    // Identity conversions for vDRMT types
    addConversion([](vdrmt::StructType type) { return type; });
    addConversion([](vdrmt::HeaderType type) { return type; });
    addConversion([](vdrmt::ReferenceType type) { return type; });

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

    // P4HIR header → vDRMT header
    addConversion([this](P4::P4MLIR::P4HIR::HeaderType p4hirHeader) -> Type {
      SmallVector<vdrmt::FieldInfo> vdrmtFields;
      
      for (const auto &p4field : p4hirHeader.getElements()) {
        Type convertedType = convertType(p4field.type);
        if (!convertedType) return Type();
        
        vdrmtFields.emplace_back(
          p4field.name,
          convertedType,
          p4field.annotations
        );
      }
      
      return vdrmt::HeaderType::get(
        p4hirHeader.getContext(),
        p4hirHeader.getName(),
        vdrmtFields,
        p4hirHeader.getAnnotations()
      );
    });
        
    // P4HIR struct → vDRMT struct
    addConversion([this](P4::P4MLIR::P4HIR::StructType p4hirStruct) -> Type {
      SmallVector<vdrmt::FieldInfo> vdrmtFields;
      
      for (const auto &p4field : p4hirStruct.getElements()) {
        Type convertedType = convertType(p4field.type);
        if (!convertedType) return Type();
        
        vdrmtFields.emplace_back(
          p4field.name,
          convertedType,
          p4field.annotations
        );
      }
      
      return vdrmt::StructType::get(
        p4hirStruct.getContext(),
        p4hirStruct.getName(),
        vdrmtFields,
        p4hirStruct.getAnnotations()
      );
    });
    
    // P4HIR ref → vDRMT ref
    addConversion([this](P4::P4MLIR::P4HIR::ReferenceType p4hirRef) -> Type {
      Type innerType = p4hirRef.getObjectType();
      Type convertedInner = convertType(innerType);
      if (!convertedInner) return Type();
      return vdrmt::ReferenceType::get(convertedInner);
    });
    
    // P4HIR extern type — identity (used symbolically by CounterCallPattern;
    // the SSA value is not emitted into vDRMT).
    addConversion([](P4::P4MLIR::P4HIR::ExternType type) -> Type {
      return type;
    });

    // P4HIR void
    addConversion([](P4::P4MLIR::P4HIR::VoidType type) -> Type {
      return type;
    });

    // Target materialization
    addTargetMaterialization([](OpBuilder &builder, Type resultType, 
                                 ValueRange inputs, Location loc) -> Value {
      if (inputs.size() != 1) return nullptr;
      if (inputs[0].getType() == resultType) return inputs[0];
      return builder.create<vdrmt::CastOp>(loc, resultType, inputs[0]);
    });

    // Source materialization
    addSourceMaterialization([](OpBuilder &builder, Type resultType,
                                ValueRange inputs, Location loc) -> Value {
      if (inputs.size() != 1) return nullptr;
      if (inputs[0].getType() == resultType) return inputs[0];
      return builder.create<vdrmt::CastOp>(loc, resultType, inputs[0]);
    });
  }
};

// ============================================================================
// Conversion Patterns
// ============================================================================

// Convert p4hir.read → vdrmt.read
class ReadOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::ReadOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::ReadOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::ReadOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    rewriter.replaceOpWithNewOp<vdrmt::ReadOp>(
      op,
      convertedType,
      adaptor.getRef()
    );
    
    return success();
  }
};

// Convert p4hir.assign → vdrmt.assign
class AssignOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::AssignOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::AssignOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::AssignOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    rewriter.replaceOpWithNewOp<vdrmt::AssignOp>(
      op,
      adaptor.getValue(),
      adaptor.getRef()
    );
    
    return success();
  }
};

// Convert p4hir.binop → vdrmt.binop
class BinOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::BinOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::BinOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::BinOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    auto p4hirKind = op.getKind();
    auto vdrmtKind = static_cast<vdrmt::BinOpKind>(static_cast<int>(p4hirKind));
    
    rewriter.replaceOpWithNewOp<vdrmt::BinOp>(
      op,
      convertedType,
      vdrmtKind,
      adaptor.getLhs(),
      adaptor.getRhs()
    );
    
    return success();
  }
};

// Convert p4hir.struct_extract → vdrmt.struct_extract
class StructExtractOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::StructExtractOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) {
      return op.emitError("failed to convert result type: ") << op.getType();
    }
    
    rewriter.replaceOpWithNewOp<vdrmt::StructExtractOp>(
      op,
      convertedType,
      adaptor.getInput(),
      op.getFieldIndexAttr()
    );
    
    return success();
  }
};

// Convert p4hir.struct_extract_ref → vdrmt.struct_extract_ref
class StructExtractRefOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractRefOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::StructExtractRefOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::StructExtractRefOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedResultType = getTypeConverter()->convertType(op.getType());
    if (!convertedResultType) return failure();
    
    rewriter.replaceOpWithNewOp<vdrmt::StructExtractRefOp>(
      op,
      convertedResultType,
      adaptor.getInput(),
      op.getFieldIndexAttr()
    );
    
    return success();
  }
};

// Convert p4hir.const → vdrmt.constant
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
    Attribute newValueAttr;
    
    if (auto p4hirIntAttr = mlir::dyn_cast<P4::P4MLIR::P4HIR::IntAttr>(valueAttr)) {
      APInt value = p4hirIntAttr.getValue();
      
      if (auto intType = mlir::dyn_cast<IntegerType>(convertedType)) {
        unsigned targetWidth = intType.getWidth();
        if (value.getBitWidth() != targetWidth) {
          value = value.zextOrTrunc(targetWidth);
        }
        newValueAttr = IntegerAttr::get(intType, value);
      } else if (mlir::isa<P4::P4MLIR::P4HIR::InfIntType>(convertedType)) {
        newValueAttr = valueAttr;
      } else {
        newValueAttr = valueAttr;
      }
    } else if (auto intAttr = mlir::dyn_cast<IntegerAttr>(valueAttr)) {
      if (auto intType = mlir::dyn_cast<IntegerType>(convertedType)) {
        APInt value = intAttr.getValue();
        unsigned targetWidth = intType.getWidth();
        if (value.getBitWidth() != targetWidth) {
          value = value.zextOrTrunc(targetWidth);
        }
        newValueAttr = IntegerAttr::get(intType, value);
      } else {
        newValueAttr = valueAttr;
      }
    } else if (auto validityAttr =
                   mlir::dyn_cast<P4::P4MLIR::P4HIR::ValidityBitAttr>(valueAttr)) {
      // validity.bit valid → 1 : i1, invalid → 0 : i1
      int64_t bitVal =
          (validityAttr.getValue() == P4::P4MLIR::P4HIR::ValidityBit::Valid) ? 1 : 0;
      newValueAttr = IntegerAttr::get(IntegerType::get(op.getContext(), 1), bitVal);
      convertedType = IntegerType::get(op.getContext(), 1);
    } else {
      newValueAttr = valueAttr;
    }

    rewriter.replaceOpWithNewOp<vdrmt::ConstantOp>(
      op,
      convertedType,
      newValueAttr
    );
    
    return success();
  }
};

// Convert p4hir.cast → vdrmt.cast
class CastOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::CastOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::CastOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::CastOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();
    
    rewriter.replaceOpWithNewOp<vdrmt::CastOp>(
      op,
      convertedType,
      adaptor.getSrc()
    );
    
    return success();
  }
};

// Convert p4hir.cmp → vdrmt.cmp
class CmpOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::CmpOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::CmpOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::CmpOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    auto i1Type = rewriter.getI1Type();
    
    rewriter.replaceOpWithNewOp<vdrmt::CmpOp>(
      op,
      i1Type,
      adaptor.getLhs(),
      adaptor.getRhs(),
      op.getKindAttr()
    );
    
    return success();
  }
};

// Convert p4hir.if → vdrmt.if with successor tracking
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
    
    Region &p4hirThen = op.getThenRegion();
    Region &p4hirElse = op.getElseRegion();
    bool hasElse = !p4hirElse.empty();
    
    // Create vDRMT if
    auto vdrmtIf = rewriter.create<vdrmt::IfOp>(
      op.getLoc(),
      adaptor.getCondition(),
      hasElse
    );

    // Clear auto-generated blocks
    vdrmtIf.getThenRegion().front().clear();
    vdrmtIf.getThenRegion().getBlocks().clear();
    
    if (hasElse) {
      vdrmtIf.getElseRegion().front().clear();
      vdrmtIf.getElseRegion().getBlocks().clear();
    }
    
    // Inline P4HIR regions
    rewriter.inlineRegionBefore(p4hirThen,
                                 vdrmtIf.getThenRegion(),
                                 vdrmtIf.getThenRegion().end());

    if (hasElse) {
      rewriter.inlineRegionBefore(p4hirElse,
                                   vdrmtIf.getElseRegion(),
                                   vdrmtIf.getElseRegion().end());
    }

    // Stash the per-branch successor IDs as temporary attributes on the if op.
    // A post-conversion walk (after applyPartialConversion) will use these to
    // replace the vdrmt.yield terminators inside each region with vdrmt.next N,
    // then remove these attrs.
    if (!exitInfo.empty()) {
      vdrmtIf->setAttr("__then_successor__",
        rewriter.getI32IntegerAttr(exitInfo[0].successor));
      if (exitInfo.size() >= 2)
        vdrmtIf->setAttr("__else_successor__",
          rewriter.getI32IntegerAttr(exitInfo[1].successor));
    }

    rewriter.eraseOp(op);
    return success();
  }

private:
  std::vector<BlockExitInfo> exitInfo;
};

// Convert p4hir.yield → vdrmt.yield
class YieldOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::YieldOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::YieldOp>::OpConversionPattern;
  
  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::YieldOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    
    rewriter.replaceOpWithNewOp<vdrmt::YieldOp>(op);
    return success();
  }
};

// Convert p4hir.scope → inline its body into the parent block.
// The scope has no results in our usage (it performs side effects only).
struct ScopeOpConversion : OpConversionPattern<P4::P4MLIR::P4HIR::ScopeOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::ScopeOp op,
                                OpAdaptor adaptor,
                                ConversionPatternRewriter &rewriter) const override {
    // Only handle scopes that produce no values.
    if (op.getNumResults() > 0) return failure();

    Region &body = op.getScopeRegion();
    if (body.empty()) {
      rewriter.eraseOp(op);
      return success();
    }

    Block &bodyBlock = body.front();
    // Remove the terminator (p4hir.yield with no values).
    Operation *term = bodyBlock.getTerminator();
    if (term) rewriter.eraseOp(term);

    // Move all remaining ops before the scope op, then erase it.
    rewriter.inlineBlockBefore(&bodyBlock, op);
    rewriter.eraseOp(op);
    return success();
  }
};

// Convert p4hir.variable → vdrmt.variable
// Needed so that out-parameter temporaries (e.g. result_out_arg for Hash5Tuple::apply)
// survive through DialectConversion and are accessible by downstream patterns.
class VariableOpConversion : public OpConversionPattern<P4::P4MLIR::P4HIR::VariableOp> {
public:
  using OpConversionPattern<P4::P4MLIR::P4HIR::VariableOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      P4::P4MLIR::P4HIR::VariableOp op,
      OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {

    Type convertedType = getTypeConverter()->convertType(op.getType());
    if (!convertedType) return failure();

    StringRef name = op.getName().value_or("tmp");
    rewriter.replaceOpWithNewOp<vdrmt::VariableOp>(
        op, convertedType, rewriter.getStringAttr(name),
        /*init=*/nullptr);
    return success();
  }
};

// ── Hash5Tuple call_method:
//   p4hir.call_method @Hash5Tuple::@apply → vdrmt.hash5tuple.apply + vdrmt.assign
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

        // Determine instance name from the base operand.
        StringRef instanceName = "unknown_hash";
        int32_t nrEntries = 64; // default
        if (auto instOp = op.getBase().getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>())
            instanceName = instOp.getName();

        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));

        // Result type of the apply op is i32 (hash value).
        auto i32Ty = rewriter.getI32Type();
        auto applyOp = rewriter.create<vdrmt::Hash5TupleApplyOp>(
            op.getLoc(), i32Ty, symRef,
            srcAddr, dstAddr, srcPort, dstPort, proto);
        // Annotate with nr_entries so VDRMTCoarseGrainedPass can emit the decl.
        applyOp->setAttr("vdrmt.hash5tuple_nr_entries",
                         rewriter.getI32IntegerAttr(nrEntries));

        // Assign the hash result into the out-parameter reference.
        rewriter.create<vdrmt::AssignOp>(op.getLoc(), applyOp.getResult(), resultRef);

        rewriter.eraseOp(op);
        return success();
    }
};

// ── Counter call_method: p4hir.call_method @Counter::@count → vdrmt.counter.count
struct CounterCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        // callee format: @Counter::@count — check root and nested references.
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty())
            return failure();

        auto rootRef   = callee.getRootReference().getValue();
        auto methodRef = callee.getNestedReferences().back().getValue();

        if (rootRef != "Counter" || methodRef != "count")
            return failure();

        // getBase() is the Counter instance SSA value.
        Value instanceVal = op.getBase();
        StringRef instanceName = "unknown_counter";
        int32_t size = 1024; // default

        if (auto instOp = instanceVal.getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            // Constructor arg[0] is the size constant.
            if (!instOp.getArgOperands().empty()) {
                if (auto cst = instOp.getArgOperands()[0]
                                   .getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>()) {
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        size = (int32_t)ia.getInt();
                }
            }
        }

        // The index argument is the first element of arg_operands.
        if (op.getArgOperands().empty())
            return failure();
        Value idx = adaptor.getArgOperands()[0];

        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));
        auto countOp = rewriter.replaceOpWithNewOp<vdrmt::CounterCountOp>(
            op, symRef, idx);
        // Annotate with instance size so VDRMTCoarseGrainedPass can emit the decl.
        countOp->setAttr("vdrmt.counter_size", rewriter.getI32IntegerAttr(size));
        return success();
    }
};

// ── Meter call_method: p4hir.call_method @meter::@execute_meter → vdrmt.meter.execute
struct MeterCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty())
            return failure();

        auto rootRef   = callee.getRootReference().getValue();
        auto methodRef = callee.getNestedReferences().back().getValue();

        if (rootRef != "meter" || methodRef != "execute_meter")
            return failure();

        // getBase() is the meter instance SSA value.
        Value instanceVal = op.getBase();
        StringRef instanceName = "unknown_meter";
        int32_t size = 1024;   // default number of slots
        int32_t meterType = 0; // 0=packets, 1=bytes

        if (auto instOp = instanceVal.getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            auto ctorArgs = instOp.getArgOperands();
            // Constructor arg[0] = size
            if (ctorArgs.size() >= 1)
                if (auto cst = ctorArgs[0].getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>())
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        size = (int32_t)ia.getInt();
            // Constructor arg[1] = MeterType enum (0=packets,1=bytes)
            if (ctorArgs.size() >= 2)
                if (auto cst = ctorArgs[1].getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>())
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        meterType = (int32_t)ia.getInt();
        }

        // arg_operands: [index, result_ref(out)]
        auto args = adaptor.getArgOperands();
        if (args.size() < 2) return failure();
        Value idx       = args[0];
        Value resultRef = args[1];

        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));
        auto i8Ty = rewriter.getIntegerType(8);
        auto execOp = rewriter.create<vdrmt::MeterExecuteOp>(
            op.getLoc(), i8Ty, symRef, idx);
        // Annotate with size and type so VDRMTCoarseGrainedPass can emit the decl.
        execOp->setAttr("vdrmt.meter_size",      rewriter.getI32IntegerAttr(size));
        execOp->setAttr("vdrmt.meter_type",      rewriter.getI32IntegerAttr(meterType));

        // Assign the color result into the out-parameter reference.
        rewriter.create<vdrmt::AssignOp>(op.getLoc(), execOp.getColor(), resultRef);

        rewriter.eraseOp(op);
        return success();
    }
};

// ── Register call_method: @Register::@read → vdrmt.register.read
//                         @Register::@write → vdrmt.register.write
struct RegisterCallPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        auto callee = op.getCallee();
        if (callee.getNestedReferences().empty())
            return failure();

        auto rootRef   = callee.getRootReference().getValue();
        auto methodRef = callee.getNestedReferences().back().getValue();

        if (rootRef != "Register")
            return failure();
        if (methodRef != "read" && methodRef != "write")
            return failure();

        Value instanceVal = op.getBase();
        StringRef instanceName = "unknown_register";
        int32_t size = 1024;
        int32_t elementWidth = 32;

        if (auto instOp = instanceVal.getDefiningOp<P4::P4MLIR::P4HIR::InstantiateOp>()) {
            instanceName = instOp.getName();
            if (!instOp.getArgOperands().empty())
                if (auto cst = instOp.getArgOperands()[0]
                                     .getDefiningOp<P4::P4MLIR::P4HIR::ConstOp>())
                    if (auto ia = dyn_cast<IntegerAttr>(cst.getValue()))
                        size = (int32_t)ia.getInt();
            // Infer element width from the instance type parameter if possible.
            // Fall back to 32 bits as the default.
            if (auto instType = instOp.getType())
                if (auto intTy = dyn_cast<IntegerType>(instType))
                    elementWidth = (int32_t)intTy.getWidth();
        }

        auto args   = adaptor.getArgOperands();
        auto symRef = FlatSymbolRefAttr::get(rewriter.getStringAttr(instanceName));

        if (methodRef == "read") {
            // read(index) → vdrmt.register.read @inst[index] : idx_ty -> elem_ty
            if (args.empty()) return failure();
            Value idx = args[0];
            auto elemTy = rewriter.getIntegerType(elementWidth);
            auto readOp = rewriter.replaceOpWithNewOp<vdrmt::RegisterReadOp>(
                op, elemTy, symRef, idx);
            readOp->setAttr("vdrmt.register_size",          rewriter.getI32IntegerAttr(size));
            readOp->setAttr("vdrmt.register_element_width", rewriter.getI32IntegerAttr(elementWidth));
        } else {
            // write(index, value) → vdrmt.register.write @inst[index] = value
            if (args.size() < 2) return failure();
            Value idx = args[0];
            Value val = args[1];
            auto writeOp = rewriter.create<vdrmt::RegisterWriteOp>(
                op.getLoc(), symRef, idx, val);
            writeOp->setAttr("vdrmt.register_size",          rewriter.getI32IntegerAttr(size));
            writeOp->setAttr("vdrmt.register_element_width", rewriter.getI32IntegerAttr(elementWidth));
            rewriter.eraseOp(op);
        }
        return success();
    }
};

// Drop p4hir.instantiate — extern instances (Counter, Hash, Meter, etc.) are handled
// symbolically by CounterCallPattern etc.; the SSA value is not needed in vDRMT.
struct InstantiateDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::InstantiateOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::InstantiateOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Drop p4hir.table_apply — the match-action table dispatch is handled by the
// vDRMT firmware itself; the lowered IR records the has_table attribute.
struct TableApplyDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::TableApplyOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::TableApplyOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Drop p4hir.call — action bodies are inlined by the partitioner; call sites
// that survive into the per-block P4HIR have no lowering in vDRMT.
struct CallOpDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// Drop p4hir.call_method for unrecognized extern calls (not Counter::count).
struct CallMethodDropPattern : OpConversionPattern<P4::P4MLIR::P4HIR::CallMethodOp> {
    using OpConversionPattern::OpConversionPattern;
    // Lower priority than CounterCallPattern (benefit = 0).
    CallMethodDropPattern(TypeConverter &tc, MLIRContext *ctx)
        : OpConversionPattern(tc, ctx, /*benefit=*/0) {}
    LogicalResult matchAndRewrite(P4::P4MLIR::P4HIR::CallMethodOp op,
                                  OpAdaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        rewriter.eraseOp(op);
        return success();
    }
};

// ============================================================================
// vDRMT Block Generator
// ============================================================================

class VDRMTBlockGenerator {
public:
  VDRMTBlockGenerator(MLIRContext *context, StringRef inputDir)
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
    
    // Generate vDRMT for each block
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
        
        if (meta.isTerminal) {
          llvm::outs() << "    ⊘ Skipping " << dirName << " (terminal block)\n";
          skipCount++;
          continue;
        }

        if (!meta.canMapToVDRMT) {
          llvm::outs() << "    ⊘ Skipping " << dirName << " (not vDRMT compatible)\n";
          skipCount++;
          continue;
        }
        
        std::string p4hirFile = path + "/p4hir.mlir";
        std::string vdrmtFile = path + "/vdrmt.mlir";
        
        if (failed(generateBlockVDRMT(p4hirFile, vdrmtFile, meta, memObjects))) {
          llvm::errs() << "    ✗ Failed to generate " << dirName << "/vdrmt.mlir\n";
          return failure();
        }
        
        successCount++;
        llvm::outs() << "    ✓ Generated " << dirName << "/vdrmt.mlir\n";
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
  P4HIRToVDRMTTypeConverter typeConverter;
  
  // Preprocess: lower p4hir.ternary(A, true { ops; yield B }, false { yield false })
  // to: [inline true-region ops] + p4hir.binop(and, A, B).
  // This is the standard P4 encoding of A && B.
  static void lowerTernaryToBinOp(func::FuncOp f) {
    using namespace P4::P4MLIR::P4HIR;

    SmallVector<TernaryOp> worklist;
    f.walk([&](TernaryOp t) { worklist.push_back(t); });

    for (auto t : llvm::reverse(worklist)) { // reverse = inner-first
      if (!t.getResult()) continue;

      // Check: false-region yields a constant 'false'.
      Region &falseReg = t.getFalseRegion();
      if (falseReg.empty()) continue;
      bool falseIsConst = false;
      for (auto &op : falseReg.front()) {
        if (auto y = dyn_cast<YieldOp>(&op)) {
          if (y.getNumOperands() == 1) {
            if (auto cst = y.getOperand(0).getDefiningOp<ConstOp>()) {
              if (auto ba = dyn_cast<P4::P4MLIR::P4HIR::BoolAttr>(cst.getValue()))
                falseIsConst = !ba.getValue(); // it's 'false'
            }
          }
        }
      }
      if (!falseIsConst) continue;

      // Find yield in true-region.
      Region &trueReg = t.getTrueRegion();
      if (trueReg.empty()) continue;
      Block &trueBlock = trueReg.front();
      YieldOp trueYield = dyn_cast<YieldOp>(trueBlock.getTerminator());
      if (!trueYield || trueYield.getNumOperands() == 0) continue;
      Value trueResult = trueYield.getOperand(0);

      // Move all non-yield ops before the ternary.
      OpBuilder builder(t);
      for (auto &op : llvm::make_early_inc_range(trueBlock.getOperations())) {
        if (&op == trueYield.getOperation()) break;
        op.moveBefore(t);
      }

      // p4hir.binop requires AnyIntP4Type (not bool). Cast bool→bit<1>, And, cast back.
      Location loc = t.getLoc();
      MLIRContext *ctx = t.getContext();
      auto b1Ty = P4::P4MLIR::P4HIR::BitsType::get(ctx, 1, /*isSigned=*/false);
      auto boolTy = t.getCond().getType();  // !p4hir.bool

      Value castCond = builder.create<CastOp>(loc, b1Ty, t.getCond());
      Value castTrue = builder.create<CastOp>(loc, b1Ty, trueResult);
      Value andBits  = builder.create<BinOp>(loc, BinOpKind::And, castCond, castTrue);
      Value andBool  = builder.create<CastOp>(loc, boolTy, andBits);

      t.getResult().replaceAllUsesWith(andBool);

      // Erase the yield and the ternary.
      trueYield.erase();
      t.erase();
    }
  }

  LogicalResult generateBlockVDRMT(StringRef p4hirFile,
                                  StringRef vdrmtFile,
                                  BlockMetadata &meta,
                                  std::vector<MemoryObjectInfo> &memObjects) {
    // Parse P4HIR module
    auto p4hirModule = parseSourceFile<ModuleOp>(p4hirFile, context);
    if (!p4hirModule) return failure();
    
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
    
    // Create vDRMT module
    auto vdrmtModule = ModuleOp::create(UnknownLoc::get(context));
    OpBuilder builder(context);
    builder.setInsertionPointToStart(vdrmtModule.getBody());
    
    // Convert function signature types
    SmallVector<Type> convertedArgTypes;
    for (auto argType : p4hirFunc.getFunctionType().getInputs()) {
      Type converted = typeConverter.convertType(argType);
      if (!converted) {
        vdrmtModule.erase();
        return failure();
      }
      convertedArgTypes.push_back(converted);
    }
    
    SmallVector<Type> convertedResultTypes;
    for (auto resultType : p4hirFunc.getFunctionType().getResults()) {
      Type converted = typeConverter.convertType(resultType);
      if (!converted) {
        vdrmtModule.erase();
        return failure();
      }
      convertedResultTypes.push_back(converted);
    }
    
    auto vdrmtFuncType = builder.getFunctionType(convertedArgTypes, convertedResultTypes);
    
    // Create vDRMT function
    auto vdrmtFunc = builder.create<func::FuncOp>(
      p4hirFunc.getLoc(),
      "vdrmt_block" + std::to_string(meta.blockId),
      vdrmtFuncType
    );
    
    // Add vDRMT metadata including successors
    vdrmtFunc->setAttr("vdrmt.block_id", builder.getI32IntegerAttr(meta.blockId));
    vdrmtFunc->setAttr("vdrmt.control_name", builder.getStringAttr(meta.controlName));
    vdrmtFunc->setAttr("vdrmt.has_table", builder.getBoolAttr(meta.hasTableApply));
    vdrmtFunc->setAttr("vdrmt.has_memory", builder.getBoolAttr(meta.hasMemoryAccess));
    
    // Add successor information as attribute
    if (!exitInfo.empty()) {
      SmallVector<int32_t> successors;
      for (auto &exit : exitInfo) {
        successors.push_back(exit.successor);
      }
      vdrmtFunc->setAttr("vdrmt.successors", 
        builder.getI32ArrayAttr(successors));
    }
    
    // Set up conversion target
    ConversionTarget target(*context);
    target.addLegalDialect<vdrmt::vDRMTDialect>();
    target.addLegalDialect<func::FuncDialect>();
    target.addIllegalDialect<P4::P4MLIR::P4HIR::P4HIRDialect>();

    // Add conversion patterns
    RewritePatternSet patterns(context);
    patterns.add<
      ReadOpConversion,
      AssignOpConversion,
      BinOpConversion,
      StructExtractOpConversion,
      StructExtractRefOpConversion,
      ConstOpConversion,
      CastOpConversion,
      CmpOpConversion,
      YieldOpConversion,
      ScopeOpConversion,
      VariableOpConversion,
      Hash5TupleCallPattern,
      CounterCallPattern,
      MeterCallPattern,
      RegisterCallPattern,
      TableApplyDropPattern,
      CallOpDropPattern,
      InstantiateDropPattern
    >(typeConverter, context);
    // Lower priority than CounterCallPattern/Hash5TupleCallPattern/MeterCallPattern so those match first.
    patterns.add<CallMethodDropPattern>(typeConverter, context);

    // Add IfOpConversion with exit info
    patterns.add<IfOpConversion>(typeConverter, context, exitInfo);
    
    // Clone the P4HIR function body
    Block *vdrmtBody = vdrmtFunc.addEntryBlock();
    IRMapping mapping;
    
    Block &p4hirEntry = p4hirFunc.getBody().front();
    for (unsigned i = 0; i < p4hirFunc.getNumArguments(); ++i) {
      mapping.map(p4hirEntry.getArgument(i), vdrmtBody->getArgument(i));
    }
    
    // Clone operations
    OpBuilder cloneBuilder(context);
    cloneBuilder.setInsertionPointToStart(vdrmtBody);
    
    for (auto &op : p4hirEntry.getOperations()) {
      cloneBuilder.clone(op, mapping);
    }

    // Preprocess: lower p4hir.ternary(A, {yield B}, {yield false}) → binop(and,A,B)
    // This must happen before DialectConversion since ternary has no conversion pattern.
    lowerTernaryToBinOp(vdrmtFunc);

    // Apply conversion
    if (failed(applyPartialConversion(vdrmtFunc, target, std::move(patterns)))) {
      vdrmtModule.erase();
      return failure();
    }

    // Post-conversion fixup pass 1: for each vdrmt.if that carries
    // __then_successor__ / __else_successor__ temp attrs, replace the
    // vdrmt.yield at the end of each region with a vdrmt.next N so that the
    // routing lives INSIDE the branch, not after it.
    vdrmtFunc.walk([&](vdrmt::IfOp ifOp) {
      auto thenAttr = ifOp->getAttrOfType<IntegerAttr>("__then_successor__");
      if (!thenAttr) return;

      // Helper: replace the terminator yield in a region with vdrmt.next N.
      auto replaceYieldWithNext = [&](Region &region, int32_t successor) {
        if (region.empty()) return;
        Block &block = region.back();
        if (block.empty()) return;
        Operation *term = &block.back();
        OpBuilder b(term);
        b.create<vdrmt::NextOp>(term->getLoc(), successor);
        // Remove the yield (or whatever terminator was there).
        if (isa<vdrmt::YieldOp>(term))
          term->erase();
      };

      replaceYieldWithNext(ifOp.getThenRegion(), (int32_t)thenAttr.getInt());

      if (auto elseAttr = ifOp->getAttrOfType<IntegerAttr>("__else_successor__"))
        replaceYieldWithNext(ifOp.getElseRegion(), (int32_t)elseAttr.getInt());

      ifOp->removeAttr("__then_successor__");
      ifOp->removeAttr("__else_successor__");
    });

    // Post-conversion fixup pass 2: for simple blocks (no vdrmt.if), insert
    // an unconditional vdrmt.next N before the function's final return.
    if (!exitInfo.empty() && !vdrmtFunc.getBody().empty()) {
      Block &lastBlock = vdrmtFunc.getBody().back();

      bool hasNext = llvm::any_of(lastBlock, [](Operation &op) {
        return isa<vdrmt::NextOp>(&op);
      });

      // If this block contains a vdrmt.if, routing is already handled inside
      // the if's then/else regions — do not add an extra unconditional next.
      bool hasIf = llvm::any_of(lastBlock, [](Operation &op) {
        return isa<vdrmt::IfOp>(&op);
      });

      if (!hasNext && !hasIf && !lastBlock.empty()) {
        Operation *terminator = &lastBlock.back();
        OpBuilder insertBuilder(terminator);
        insertBuilder.create<vdrmt::NextOp>(
          terminator->getLoc(),
          (int32_t)exitInfo[0].successor
        );
      }
    }
      
    // Write to file
    std::error_code ec;
    llvm::raw_fd_ostream file(vdrmtFile, ec);
    if (ec) {
      vdrmtModule.erase();
      return failure();
    }
    
    OpPrintingFlags flags;
    flags.printGenericOpForm(false);
    flags.enableDebugInfo(false, false);
    
    vdrmtModule.print(file, flags);
    vdrmtModule.erase();
    
    return success();
  }
};

// ============================================================================
// P4HIR to vDRMT Lowering Pass
// ============================================================================

struct P4HIRToVDRMTPass 
  : public PassWrapper<P4HIRToVDRMTPass, OperationPass<ModuleOp>> {
  
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(P4HIRToVDRMTPass)
  
  P4HIRToVDRMTPass() = default;
  P4HIRToVDRMTPass(const P4HIRToVDRMTPass &other) : PassWrapper(other) {}
  
  std::string inputDir = "vdsa_output";
  
  StringRef getArgument() const final { return "p4hir-to-vdrmt"; }
  
  StringRef getDescription() const final {
    return "Lower partitioned P4HIR to vDRMT (generates vdrmt.mlir per block)";
  }
  
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<vdrmt::vDRMTDialect>();
    registry.insert<func::FuncDialect>();
    registry.insert<LLVM::LLVMDialect>();
  }
  
  void runOnOperation() override {
    auto *context = &getContext();
    
    llvm::outs() << "\n";
    llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    llvm::outs() << "🎯 P4HIR to vDRMT Lowering Pass\n";
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
    
    VDRMTBlockGenerator generator(context, inputDir);
    
    if (failed(generator.generate())) {
      llvm::errs() << "Failed to generate vDRMT files\n";
      signalPassFailure();
      return;
    }
    
    llvm::outs() << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    llvm::outs() << "✅ vDRMT lowering complete!\n";
    llvm::outs() << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
  }
};

} // anonymous namespace

namespace mlir {

std::unique_ptr<Pass> createP4HIRToVDRMTPass() {
  return std::make_unique<P4HIRToVDRMTPass>();
}

void registerP4HIRToVDRMTPass() {
  PassRegistration<P4HIRToVDRMTPass>();
}

} // namespace mlir