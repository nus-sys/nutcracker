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

using namespace mlir;
using namespace mlir::LLVM;
using namespace mlir::vdrmt;

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

#define GET_OP_CLASSES
#include "Dialect/vDRMT/IR/vDRMTDialect.cpp.inc"
#include "Dialect/vDRMT/IR/vDRMTOps.cpp.inc"
#include "Dialect/vDRMT/IR/vDRMTOpsEnums.cpp.inc"