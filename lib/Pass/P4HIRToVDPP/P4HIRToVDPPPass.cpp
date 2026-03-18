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

#include "Pass/P4HIRToVDPPPass.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h" 
#include "p4mlir/Dialect/P4HIR/P4HIR_Ops.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Types.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

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
    
    if (auto p4hirIntAttr = valueAttr.dyn_cast<P4::P4MLIR::P4HIR::IntAttr>()) {
      APInt value = p4hirIntAttr.getValue();
      
      if (auto intType = convertedType.dyn_cast<IntegerType>()) {
        if (value.getBitWidth() != intType.getWidth()) {
          value = value.zextOrTrunc(intType.getWidth());
        }
        auto newAttr = IntegerAttr::get(intType, value);
        rewriter.replaceOpWithNewOp<vdpp::ConstantOp>(op, convertedType, newAttr);
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
    
    // Map P4HIR BinOpKind to vDPP operations
    // Based on P4HIR enum values
    switch (kind) {
      case 0: // Add
        rewriter.replaceOpWithNewOp<vdpp::AddOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 1: // Sub
        rewriter.replaceOpWithNewOp<vdpp::SubOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 2: // Mul
        rewriter.replaceOpWithNewOp<vdpp::MulOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 3: // Div (signed)
        rewriter.replaceOpWithNewOp<vdpp::DivOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 4: // Mod/Rem
        // vDPP doesn't have mod, skip for now
        return failure();
      case 5: // Shl
        rewriter.replaceOpWithNewOp<vdpp::ShlOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 6: // Shr (logical)
        rewriter.replaceOpWithNewOp<vdpp::LShrOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 7: // Shr (arithmetic)
        rewriter.replaceOpWithNewOp<vdpp::AShrOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 8: // BAnd (bitwise and) - This is what you need!
      case 10: // And (also bitwise and)
        rewriter.replaceOpWithNewOp<vdpp::AndOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 9: // BOr (bitwise or)
      case 11: // Or
        rewriter.replaceOpWithNewOp<vdpp::OrOp>(
          op, convertedType, adaptor.getLhs(), adaptor.getRhs());
        break;
      case 12: // Xor
        rewriter.replaceOpWithNewOp<vdpp::XorOp>(
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
    
    // Mark func.return as illegal so it gets converted to vdpp.return
    target.addIllegalOp<func::ReturnOp>();
    
    // Add conversion patterns
    RewritePatternSet patterns(context);
    patterns.add<
      ConstOpConversion,
      BinOpConversion,
      CmpOpConversion,
      CastOpConversion,
      ReadOpConversion,
      AssignOpConversion,
      StructExtractOpConversion,
      StructExtractRefOpConversion
    >(typeConverter, context);
    
    // Add IfOpConversion and ReturnOpConversion with exit info
    patterns.add<IfOpConversion>(typeConverter, context, exitInfo);
    patterns.add<ReturnOpConversion>(typeConverter, context, exitInfo);  // ← Pass exitInfo here!
    
    // Create entry block
    Block *vdppBody = vdppFunc.addEntryBlock();
    IRMapping mapping;
    
    Block &p4hirEntry = p4hirFunc.getBody().front();
    for (unsigned i = 0; i < p4hirFunc.getNumArguments(); ++i) {
      mapping.map(p4hirEntry.getArgument(i), vdppBody->getArgument(i));
    }
    
    // Clone operations
    OpBuilder bodyBuilder(context);
    bodyBuilder.setInsertionPointToStart(vdppBody);
    
    for (auto &op : p4hirEntry.getOperations()) {
      bodyBuilder.clone(op, mapping);
    }
    
    // Apply conversion
    if (failed(applyPartialConversion(vdppFunc, target, std::move(patterns)))) {
      vdppModule.erase();
      return failure();
    }
    
    // NEW: Post-process to add successor to final return if it doesn't have one
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