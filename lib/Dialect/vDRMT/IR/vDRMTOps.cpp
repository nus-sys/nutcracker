//===- vDRMTOps.cpp - Virtual DRMT dialect ops ---------------*- C++ -*-===//
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

#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Types.h"

using namespace mlir;
using namespace mlir::LLVM;
using namespace mlir::vdrmt;

template <typename AggType>
static void printExtractOp(OpAsmPrinter &printer, AggType op) {
    printer << " ";
    printer.printOperand(op.getInput());
    printer << "[\"" << op.getFieldName() << "\"]";
    printer.printOptionalAttrDict(op->getAttrs(), {"fieldIndex"});
    printer << " : ";

    auto type = op.getInput().getType();
    if (auto validType = mlir::dyn_cast<vdrmt::ReferenceType>(type))
        printer.printStrippedAttrOrType(validType);
    else
        printer << type;
}

static ParseResult parseExtractRefOp(OpAsmParser &parser, OperationState &result) {
    OpAsmParser::UnresolvedOperand operand;
    StringAttr fieldName;
    vdrmt::ReferenceType declType;  // vDRMT reference type!

    if (parser.parseOperand(operand) || 
        parser.parseLSquare() || 
        parser.parseAttribute(fieldName) ||
        parser.parseRSquare() || 
        parser.parseOptionalAttrDict(result.attributes) ||
        parser.parseColon() || 
        parser.parseCustomTypeWithFallback<vdrmt::ReferenceType>(declType))
        return failure();

    auto aggType = mlir::dyn_cast<vDRMT_StructLikeTypeInterface>(declType.getObjectType());
    if (!aggType) {
        parser.emitError(parser.getNameLoc(), "expected reference to aggregate type");
        return failure();
    }
    
    auto fieldIndex = aggType.getFieldIndex(fieldName);
    if (!fieldIndex) {
        parser.emitError(parser.getNameLoc(),
                         "field name '" + fieldName.getValue() + "' not found in aggregate type");
        return failure();
    }

    auto indexAttr = parser.getBuilder().getI32IntegerAttr(*fieldIndex);
    result.addAttribute("fieldIndex", indexAttr);
    
    Type resultType = vdrmt::ReferenceType::get(aggType.getFields()[*fieldIndex].type);
    result.addTypes(resultType);

    if (parser.resolveOperand(operand, declType, result.operands)) 
        return failure();
    
    return success();
}


template <typename AggregateOp>
static LogicalResult verifyAggregateFieldIndexAndType(AggregateOp &op,
                                                      vdrmt::vDRMT_StructLikeTypeInterface aggType,
                                                      Type elementType) {
    auto index = op.getFieldIndex();
    auto fields = aggType.getFields();
    if (index >= fields.size())
        return op.emitOpError() << "field index " << index
                                << " exceeds element count of aggregate type";

    if (elementType != fields[index].type)
        return op.emitOpError() << "type " << fields[index].type
                                << " of accessed field in aggregate at index " << index
                                << " does not match expected type " << elementType;

    return success();
}

// Check if a region's termination omission is valid and, if so, creates and
// inserts the omitted terminator into the region.
static LogicalResult ensureRegionTerm(OpAsmParser &parser, Region &region, SMLoc errLoc) {
    Location eLoc = parser.getEncodedSourceLoc(parser.getCurrentLocation());
    OpBuilder builder(parser.getBuilder().getContext());

    // Insert empty block in case the region is empty to ensure the terminator
    // will be inserted
    if (region.empty()) builder.createBlock(&region);

    Block &block = region.back();
    // Region is properly terminated: nothing to do.
    if (!block.empty() && block.back().hasTrait<OpTrait::IsTerminator>()) return success();

    // Check for invalid terminator omissions.
    if (!region.hasOneBlock())
        return parser.emitError(errLoc, "multi-block region must not omit terminator");

    // Terminator was omitted correctly: recreate it.
    builder.setInsertionPointToEnd(&block);
    builder.create<vdrmt::YieldOp>(eLoc);
    return success();
}

// True if the region's terminator should be omitted when printing.
// With NoTerminator regions (vdrmt.if), blocks may legally have no terminator-
// trait op — e.g. when vdrmt.next (a Pure op) is the last op.  In that case
// there is nothing to omit, so return false.
static bool omitRegionTerm(mlir::Region &r) {
    if (!r.hasOneBlock() || r.back().empty())
        return false;
    // Only omit if the block actually has a terminator-trait op and that op
    // is a bare YieldOp (no operands).
    if (!r.back().mightHaveTerminator())
        return false;
    auto y = dyn_cast<vdrmt::YieldOp>(r.back().getTerminator());
    return y != nullptr;
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

void vdrmt::ConstantOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
    // Since there's no name attribute, just use a generic name
    setNameFn(getResult(), "cst");
}

LogicalResult vdrmt::ConstantOp::verify() {
    return success();
}

OpFoldResult vdrmt::ConstantOp::fold(FoldAdaptor adaptor) { return getValue(); }

//===----------------------------------------------------------------------===//
// BinaryOp
//===----------------------------------------------------------------------===//

void vdrmt::BinOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
    setNameFn(getResult(), stringifyEnum(getKind()));
}

//===----------------------------------------------------------------------===//
// ReadOp
//===----------------------------------------------------------------------===//

void vdrmt::ReadOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
    setNameFn(getResult(), "read");
}

//===----------------------------------------------------------------------===//
// TableOp
//===----------------------------------------------------------------------===//

void vdrmt::TableOp::build(
    mlir::OpBuilder &builder, mlir::OperationState &result, llvm::StringRef name,
    mlir::DictionaryAttr annotations,
    llvm::function_ref<void(mlir::OpBuilder &, mlir::Location)> entryBuilder) {
    result.addAttribute(getSymNameAttrName(result.name), builder.getStringAttr(name));
    
    if (annotations && !annotations.empty())
        result.addAttribute(getAnnotationsAttrName(result.name), annotations);

    OpBuilder::InsertionGuard guard(builder);

    Region *entryRegion = result.addRegion();
    builder.createBlock(entryRegion);
    entryBuilder(builder, result.location);
}

//===----------------------------------------------------------------------===//
// TableActionOp
//===----------------------------------------------------------------------===//

void vdrmt::TableActionOp::build(
    mlir::OpBuilder &builder, mlir::OperationState &result, mlir::SymbolRefAttr action,
    vdrmt::FuncType cplaneType, ArrayRef<mlir::DictionaryAttr> argAttrs,
    mlir::DictionaryAttr annotations,
    llvm::function_ref<void(mlir::OpBuilder &, mlir::Block::BlockArgListType, mlir::Location)>
        entryBuilder) {
    result.addAttribute(getCplaneTypeAttrName(result.name), TypeAttr::get(cplaneType));
    result.addAttribute(getActionAttrName(result.name), action);

    if (annotations && !annotations.empty())
        result.addAttribute(getAnnotationsAttrName(result.name), annotations);

    function_interface_impl::addArgAndResultAttrs(builder, result, argAttrs,
                                                  /*resultAttrs=*/std::nullopt,
                                                  getArgAttrsAttrName(result.name), {});

    OpBuilder::InsertionGuard guard(builder);
    auto *body = result.addRegion();

    Block &first = body->emplaceBlock();
    for (auto argType : cplaneType.getInputs()) first.addArgument(argType, result.location);
    builder.setInsertionPointToStart(&first);
    entryBuilder(builder, first.getArguments(), result.location);
}

void vdrmt::TableActionOp::print(mlir::OpAsmPrinter &printer) {
    auto actName = getActionAttr();

    printer << " ";
    printer << actName;

    printer << '(';
    const auto argTypes = getCplaneType().getInputs();
    mlir::ArrayAttr argAttrs = getArgAttrsAttr();
    for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
        if (i > 0) printer << ", ";

        ArrayRef<NamedAttribute> attrs;
        if (argAttrs) attrs = llvm::cast<DictionaryAttr>(argAttrs[i]).getValue();
        printer.printRegionArgument(getBody().front().getArgument(i), attrs);
    }
    printer << ')';

    function_interface_impl::printFunctionAttributes(
        printer, *this,
        // These are all omitted since they are custom printed already.
        {getActionAttrName(), getCplaneTypeAttrName(), getAnnotationsAttrName(),
         getArgAttrsAttrName()});

    if (auto ann = getAnnotations(); ann && !ann->empty()) {
        printer << " annotations ";
        printer.printAttributeWithoutType(*ann);
    }

    printer << ' ';
    printer.printRegion(getRegion(), /*printEntryBlockArgs=*/false, /*printBlockTerminators=*/true);
}

mlir::ParseResult vdrmt::TableActionOp::parse(mlir::OpAsmParser &parser,
                                              mlir::OperationState &result) {
    // This is essentially function_interface_impl::parseFunctionOp, but we do not have
    // result / argument attributes (for now)
    llvm::SMLoc loc = parser.getCurrentLocation();
    auto &builder = parser.getBuilder();

    // Parse the name as a symbol.
    SymbolRefAttr actionAttr;
    if (parser.parseCustomAttributeWithFallback(actionAttr, builder.getType<::mlir::NoneType>(),
                                                getActionAttrName(result.name), result.attributes))
        return mlir::failure();

    llvm::SmallVector<OpAsmParser::Argument, 8> arguments;
    llvm::SmallVector<DictionaryAttr, 1> resultAttrs;
    llvm::SmallVector<Type, 8> argTypes;
    llvm::SmallVector<Type, 0> resultTypes;
    bool isVariadic = false;
    if (function_interface_impl::parseFunctionSignature(parser, /*allowVariadic=*/false, arguments,
                                                        isVariadic, resultTypes, resultAttrs))
        return mlir::failure();

    // Table actions have no results
    if (!resultTypes.empty())
        return parser.emitError(loc, "table actions should not produce any results");

    // Build the function type.
    for (auto &arg : arguments) argTypes.push_back(arg.type);

    if (auto fnType = vdrmt::FuncType::get(builder.getContext(), argTypes)) {
        result.addAttribute(getCplaneTypeAttrName(result.name), TypeAttr::get(fnType));
    } else
        return mlir::failure();

    // If additional attributes are present, parse them.
    if (parser.parseOptionalAttrDictWithKeyword(result.attributes)) return failure();

    // Add the attributes to the function arguments.
    assert(resultAttrs.size() == resultTypes.size());
    function_interface_impl::addArgAndResultAttrs(builder, result, arguments, resultAttrs,
                                                  getArgAttrsAttrName(result.name), {});

    // Parse annotations
    mlir::DictionaryAttr annotations;
    if (::mlir::succeeded(parser.parseOptionalKeyword("annotations"))) {
        if (parser.parseAttribute<mlir::DictionaryAttr>(annotations)) return failure();
        result.addAttribute(getAnnotationsAttrName(result.name), annotations);
    }

    // Parse the body.
    auto *body = result.addRegion();
    if (parser.parseRegion(*body, arguments, /*enableNameShadowing=*/false)) return mlir::failure();

    // Make sure its not empty.
    if (body->empty()) return parser.emitError(loc, "expected non-empty table action body");

    return mlir::success();
}

//===----------------------------------------------------------------------===//
// StructExtractRefOp
//===----------------------------------------------------------------------===//

void vdrmt::StructExtractRefOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
    llvm::SmallString<16> name = getFieldName();
    name += "_field_ref";
    setNameFn(getResult(), name);
}

ParseResult vdrmt::StructExtractRefOp::parse(OpAsmParser &parser, OperationState &result) {
    llvm::outs() << "Parsing StructExtractRefOp\n";
    return parseExtractRefOp(parser, result);
}

void vdrmt::StructExtractRefOp::print(OpAsmPrinter &printer) { 
    printExtractOp(printer, *this); 
}

LogicalResult vdrmt::StructExtractRefOp::verify() {
    auto type = mlir::cast<vDRMT_StructLikeTypeInterface>(
    mlir::cast<ReferenceType>(getInput().getType()).getObjectType());
    return verifyAggregateFieldIndexAndType(*this, type, getType().getObjectType());
}

void vdrmt::StructExtractRefOp::build(OpBuilder &builder, OperationState &odsState, Value input,
                                      vdrmt::FieldInfo field) {
    auto structLikeType = mlir::cast<ReferenceType>(input.getType()).getObjectType();
    auto structType = mlir::cast<vDRMT_StructLikeTypeInterface>(structLikeType);
    auto fieldIndex = structType.getFieldIndex(field.name);
    assert(fieldIndex.has_value() && "field name not found in aggregate type");
    build(builder, odsState, ReferenceType::get(field.type), input, *fieldIndex);
}

void vdrmt::StructExtractRefOp::build(OpBuilder &builder, OperationState &odsState, Value input,
                                      StringAttr fieldName) {
    auto structLikeType = mlir::cast<ReferenceType>(input.getType()).getObjectType();
    auto structType = mlir::cast<vDRMT_StructLikeTypeInterface>(structLikeType);
    auto fieldIndex = structType.getFieldIndex(fieldName);
    auto fieldType = structType.getFieldType(fieldName);
    assert(fieldIndex.has_value() && "field name not found in aggregate type");
    build(builder, odsState, ReferenceType::get(fieldType), input, *fieldIndex);
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

ParseResult vdrmt::IfOp::parse(OpAsmParser &parser, OperationState &result) {
    // Create the regions for 'then' and 'else'
    result.regions.reserve(2);
    Region *thenRegion = result.addRegion();
    Region *elseRegion = result.addRegion();

    auto &builder = parser.getBuilder();
    OpAsmParser::UnresolvedOperand cond;
    Type boolType = builder.getI1Type();

    if (parser.parseOperand(cond) || 
        parser.resolveOperand(cond, boolType, result.operands))
        return failure();

    // Parse the 'then' region
    auto parseThenLoc = parser.getCurrentLocation();
    if (parser.parseRegion(*thenRegion, /*arguments=*/{}, /*argTypes=*/{}))
        return failure();
    
    if (ensureRegionTerm(parser, *thenRegion, parseThenLoc).failed()) 
        return failure();

    // If we find an 'else' keyword, parse the 'else' region
    if (!parser.parseOptionalKeyword("else")) {
        auto parseElseLoc = parser.getCurrentLocation();
        
        if (parser.parseRegion(*elseRegion, /*arguments=*/{}, /*argTypes=*/{})) 
            return failure();
        
        if (ensureRegionTerm(parser, *elseRegion, parseElseLoc).failed()) 
            return failure();
    }

    // Parse the optional attribute list
    if (parser.parseOptionalAttrDict(result.attributes))
        return failure();
    
    return success();
}

void vdrmt::IfOp::print(OpAsmPrinter &p) {
    p << " " << getCondition();
    p << ' ';
    
    auto &thenRegion = this->getThenRegion();
    p.printRegion(thenRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/!omitRegionTerm(thenRegion));

    // Print the 'else' region if it exists and has a block
    auto &elseRegion = this->getElseRegion();
    if (!elseRegion.empty()) {
        p << " else ";
        p.printRegion(elseRegion,
                      /*printEntryBlockArgs=*/false,
                      /*printBlockTerminators=*/!omitRegionTerm(elseRegion));
    }

    p.printOptionalAttrDict(getOperation()->getAttrs());
}

/// Default callback for IfOp builders - add terminator if needed
static void buildTerminatedBody(OpBuilder &builder, Location loc) {
    Block *block = builder.getBlock();

    // Region is properly terminated: nothing to do
    if (block && block->mightHaveTerminator()) 
        return;

    // Add vdrmt.yield to the end of the block
    builder.create<vdrmt::YieldOp>(loc);
}

void vdrmt::IfOp::getSuccessorRegions(mlir::RegionBranchPoint point,
                                      SmallVectorImpl<RegionSuccessor> &regions) {
    // The `then` and the `else` region branch back to the parent operation
    if (!point.isParent()) {
        regions.push_back(RegionSuccessor());
        return;
    }

    // Don't consider the else region if it is empty
    Region *elseRegion = &this->getElseRegion();
    if (elseRegion->empty()) 
        elseRegion = nullptr;

    // If the condition isn't constant, both regions may be executed
    regions.push_back(RegionSuccessor(&getThenRegion()));
    
    // If the else region does not exist, it is not a viable successor
    if (elseRegion) 
        regions.push_back(RegionSuccessor(elseRegion));
}

// IMPLEMENTATION 1: Simple builder (auto-add terminators)
// This is what TableGen expects from the first builder declaration
void vdrmt::IfOp::build(OpBuilder &builder, OperationState &result, 
                        Value cond, bool withElseRegion) {
    result.addOperands(cond);

    OpBuilder::InsertionGuard guard(builder);
    
    // Build 'then' region with auto-terminator
    Region *thenRegion = result.addRegion();
    builder.createBlock(thenRegion);
    buildTerminatedBody(builder, result.location);

    // Build 'else' region if requested
    Region *elseRegion = result.addRegion();
    if (withElseRegion) {
        builder.createBlock(elseRegion);
        buildTerminatedBody(builder, result.location);
    }
}

// IMPLEMENTATION 2: Advanced builder (custom callbacks)
// This matches the second builder declaration in TableGen
void vdrmt::IfOp::build(OpBuilder &builder, OperationState &result, 
                        Value cond, bool withElseRegion,
                        function_ref<void(OpBuilder &, Location)> thenBuilder,
                        function_ref<void(OpBuilder &, Location)> elseBuilder) {
    assert(thenBuilder && "the builder callback for 'then' must be present");

    result.addOperands(cond);

    OpBuilder::InsertionGuard guard(builder);
    
    // Build 'then' region with custom builder
    Region *thenRegion = result.addRegion();
    builder.createBlock(thenRegion);
    thenBuilder(builder, result.location);

    // Build 'else' region if requested
    Region *elseRegion = result.addRegion();
    if (withElseRegion && elseBuilder) {
        builder.createBlock(elseRegion);
        elseBuilder(builder, result.location);
    }
}

//===----------------------------------------------------------------------===//
// NextOp
//===----------------------------------------------------------------------===//

mlir::LogicalResult vdrmt::NextOp::verify() {
    bool hasCondition = static_cast<bool>(getCondition());
    bool hasElse = getElseSuccessor().has_value();
    if (hasCondition && !hasElse)
        return emitOpError(
            "conditional next requires 'else_successor' to be set");
    if (!hasCondition && hasElse)
        return emitOpError(
            "unconditional next must not have 'else_successor'");
    return mlir::success();
}

// CompressOp / DecompressOp: $output_len must reference an integer slot so
// the BF lowering can write the produced byte count back through it.
static mlir::LogicalResult verifyVdrmtOutputLenRef(mlir::Operation *op,
                                                   mlir::Value outputLen) {
    auto refTy = mlir::dyn_cast<vdrmt::ReferenceType>(outputLen.getType());
    if (!refTy)
        return op->emitOpError("output_len must be a vdrmt.ref type");
    if (!refTy.getObjectType().isIntOrIndex())
        return op->emitOpError(
            "output_len must reference an integer/index slot, got ")
            << refTy.getObjectType();
    return mlir::success();
}

mlir::LogicalResult vdrmt::CompressOp::verify() {
    return verifyVdrmtOutputLenRef(*this, getOutputLen());
}

mlir::LogicalResult vdrmt::DecompressOp::verify() {
    return verifyVdrmtOutputLenRef(*this, getOutputLen());
}

#define GET_OP_CLASSES
#include "Dialect/vDRMT/IR/vDRMTDialect.cpp.inc"
#include "Dialect/vDRMT/IR/vDRMTOps.cpp.inc"
#include "Dialect/vDRMT/IR/vDRMTOpsEnums.cpp.inc"