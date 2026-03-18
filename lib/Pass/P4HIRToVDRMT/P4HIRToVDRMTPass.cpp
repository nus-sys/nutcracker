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
    
    // NEW: Add successor attributes to the if operation itself
    if (!exitInfo.empty() && exitInfo.size() >= 1) {
      vdrmtIf->setAttr("then_successor", 
        rewriter.getI32IntegerAttr(exitInfo[0].successor));
      
      if (hasElse && exitInfo.size() >= 2) {
        vdrmtIf->setAttr("else_successor", 
          rewriter.getI32IntegerAttr(exitInfo[1].successor));
      }
    }
    
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
      YieldOpConversion
    >(typeConverter, context);
    
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

    // Apply conversion
    if (failed(applyPartialConversion(vdrmtFunc, target, std::move(patterns)))) {
      vdrmtModule.erase();
      return failure();
    }

    // NEW: Post-process to add successor to final return
    if (!exitInfo.empty()) {
      // Check if function ends with a return (no if)
      if (!vdrmtFunc.getBody().empty()) {
        Block &lastBlock = vdrmtFunc.getBody().back();
        if (!lastBlock.empty()) {
          Operation &lastOp = lastBlock.back();
          if (auto returnOp = dyn_cast<func::ReturnOp>(&lastOp)) {
            // This is a simple block with no branching
            // Add successor annotation
            int successor = exitInfo[0].successor;
            returnOp->setAttr("successor", builder.getI32IntegerAttr(successor));
          }
        }
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