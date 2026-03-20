//===- BF3DRMTOps.cpp - BF3 NIC ASIC DRMT dialect ops --------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTTypes.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/BuiltinTypes.h"

#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.cpp.inc"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOpsEnums.cpp.inc"

#define GET_OP_CLASSES
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.cpp.inc"

using namespace mlir;
using namespace mlir::bf3drmt;

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

mlir::LogicalResult ConstantOp::verify() {
    auto typedAttr = mlir::dyn_cast<mlir::TypedAttr>(getValue());
    if (!typedAttr)
        return emitOpError("value must be a typed attribute");
    if (typedAttr.getType() != getRes().getType())
        return emitOpError("value type '") << typedAttr.getType()
               << "' does not match result type '" << getRes().getType() << "'";
    return mlir::success();
}

mlir::OpFoldResult ConstantOp::fold(FoldAdaptor) {
    return getValue();
}

void ConstantOp::getAsmResultNames(
        llvm::function_ref<void(mlir::Value, mlir::StringRef)> setNameFn) {
    setNameFn(getRes(), "cst");
}

//===----------------------------------------------------------------------===//
// ReadOp
//
// Hardware constraint: ref must be produced by struct_extract_ref or variable.
// Doing read(struct_val) -> struct_extract (by value) is not supported on the
// BF3 NIC ASIC. All field accesses must go through references.
//===----------------------------------------------------------------------===//

mlir::LogicalResult ReadOp::verify() {
    mlir::Operation *defOp = getRef().getDefiningOp();
    if (!defOp) {
        // Block argument — allowed (function parameters are references)
        return mlir::success();
    }
    if (!mlir::isa<StructExtractRefOp>(defOp) &&
        !mlir::isa<VariableOp>(defOp)) {
        return emitOpError(
            "ref operand must be produced by 'bf3drmt.struct_extract_ref' or "
            "'bf3drmt.variable' (BF3 NIC ASIC constraint: struct_extract_ref -> "
            "read is the only valid field access pattern; read -> struct_extract "
            "is not supported)");
    }
    return mlir::success();
}

void ReadOp::getAsmResultNames(
        llvm::function_ref<void(mlir::Value, mlir::StringRef)> setNameFn) {
    setNameFn(getResult(), "read");
}

//===----------------------------------------------------------------------===//
// StructExtractRefOp
//===----------------------------------------------------------------------===//

mlir::LogicalResult StructExtractRefOp::verify() {
    auto inputTy = getInput().getType();
    if (!mlir::isa<mlir::vdrmt::ReferenceType>(inputTy) &&
        !mlir::isa<mlir::bf3drmt::ReferenceType>(inputTy)) {
        return emitOpError("input must be a !vdrmt.ref<...> or !bf3drmt.ref<...> "
                           "reference type, got '")
               << inputTy << "'";
    }
    auto resultTy = getResult().getType();
    if (!mlir::isa<mlir::vdrmt::ReferenceType>(resultTy) &&
        !mlir::isa<mlir::bf3drmt::ReferenceType>(resultTy)) {
        return emitOpError("result must be a !vdrmt.ref<...> or !bf3drmt.ref<...> "
                           "reference type, got '")
               << resultTy << "'";
    }
    return mlir::success();
}

void StructExtractRefOp::getAsmResultNames(
        llvm::function_ref<void(mlir::Value, mlir::StringRef)> setNameFn) {
    setNameFn(getResult(), "field_ref");
}

mlir::ParseResult StructExtractRefOp::parse(mlir::OpAsmParser &parser,
                                                      mlir::OperationState &result) {
    mlir::OpAsmParser::UnresolvedOperand input;
    mlir::IntegerAttr fieldIndex;
    mlir::Type inputType, resultType;

    if (parser.parseOperand(input) ||
        parser.parseLSquare() ||
        parser.parseAttribute(fieldIndex, parser.getBuilder().getI32Type()) ||
        parser.parseRSquare() ||
        parser.parseColon() ||
        parser.parseType(inputType) ||
        parser.resolveOperand(input, inputType, result.operands))
        return mlir::failure();

    result.addAttribute("fieldIndex", fieldIndex);

    if (parser.parseOptionalAttrDict(result.attributes))
        return mlir::failure();

    if (parser.parseArrow() || parser.parseType(resultType))
        return mlir::failure();

    result.addTypes(resultType);
    return mlir::success();
}

void StructExtractRefOp::print(mlir::OpAsmPrinter &printer) {
    printer << " " << getInput()
            << "[" << getFieldIndex() << "]"
            << " : " << getInput().getType();
    printer.printOptionalAttrDict((*this)->getAttrs(), {"fieldIndex"});
    printer << " -> " << getResult().getType();
}

//===----------------------------------------------------------------------===//
// BinOp
//
// Hardware constraints:
//   - Mul and Div are absent (lowered to SHL/SHR by the egglog pass).
//   - Sub requires a constant rhs (implemented as ADD overflow on the NIC).
//===----------------------------------------------------------------------===//

mlir::LogicalResult BinOp::verify() {
    if (getKind() == BF3DRMTBinOpKind::Sub) {
        mlir::Operation *rhsDef = getRhs().getDefiningOp();
        if (!rhsDef || !mlir::isa<ConstantOp>(rhsDef)) {
            return emitOpError(
                "Sub operation requires the rhs operand to be a constant "
                "(BF3 NIC ASIC implements Sub as ADD overflow with constant rhs)");
        }
    }
    return mlir::success();
}

void BinOp::getAsmResultNames(
        llvm::function_ref<void(mlir::Value, mlir::StringRef)> setNameFn) {
    setNameFn(getResult(), "binop");
}

//===----------------------------------------------------------------------===//
// TableKeyOp
//
// Verifier: the body must end with a bf3drmt.match_key op.
//===----------------------------------------------------------------------===//

mlir::LogicalResult TableKeyOp::verify() {
    auto &body = getBody();
    if (body.empty())
        return emitOpError("table_key body must not be empty");
    mlir::Block &lastBlock = body.back();
    if (lastBlock.empty() || !mlir::isa<MatchKeyOp>(lastBlock.back()))
        return emitOpError(
            "table_key body must end with a 'bf3drmt.match_key' op");
    return mlir::success();
}

//===----------------------------------------------------------------------===//
// PipeOp
//===----------------------------------------------------------------------===//

mlir::LogicalResult PipeOp::verify() {
    auto ptype = getPipeType();
    // match_mask is only meaningful for basic and acl pipes.
    if (getMatchMask().has_value()) {
        if (ptype != BF3DRMTPipeType::Basic && ptype != BF3DRMTPipeType::Acl) {
            return emitOpError(
                "match_mask is only valid for 'basic' and 'acl' pipe types");
        }
    }
    // nr_entries required for hash pipe.
    if (ptype == BF3DRMTPipeType::Hash && !getNrEntries().has_value()) {
        return emitOpError("'hash' pipe requires 'nr_entries' to be set");
    }
    return mlir::success();
}

//===----------------------------------------------------------------------===//
// PipeActionOp
//
// The body must end with a bf3drmt.next op (the hit successor). Forward
// routing is no longer expressed as attributes — it is expressed via the
// next terminator inside the body.
//===----------------------------------------------------------------------===//

mlir::LogicalResult PipeActionOp::verify() {
    auto &bodyRegion = getBody();
    if (bodyRegion.empty())
        return emitOpError("pipe_action body must not be empty");

    mlir::Block &lastBlock = bodyRegion.back();
    if (lastBlock.empty() || !mlir::isa<NextOp>(lastBlock.back())) {
        return emitOpError(
            "pipe_action body must end with a 'bf3drmt.next' op specifying "
            "the hit successor block");
    }
    return mlir::success();
}
