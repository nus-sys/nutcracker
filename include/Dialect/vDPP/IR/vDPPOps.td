//===- vDPPOps.td - vDPP dialect ops -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDPPOPS
#define VDPPOPS

include "mlir/IR/OpBase.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/LoopLikeInterface.td"
include "mlir/Interfaces/MemorySlotInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/CastInterfaces.td"
include "mlir/Interfaces/InferIntRangeInterface.td"
include "mlir/Interfaces/VectorInterfaces.td"
include "mlir/Dialect/LLVMIR/LLVMOpBase.td"

include "Dialect/vDPP/IR/vDPPDialect.td"
include "Dialect/vDPP/IR/vDPPTypes.td"

//===----------------------------------------------------------------------===//
// Base vDPP operation definition.
//===----------------------------------------------------------------------===//

class vDPP_Op<string mnemonic, list<Trait> traits = []> :
        Op<vDPP_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// vDPP operation definitions.
//===----------------------------------------------------------------------===//

def ConstantOp : vDPP_Op<"constant",
    [ConstantLike, Pure,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Defines a vDPP constant";

    let arguments = (ins AnyAttr:$value);
    let results = (outs AnyType:$res);

    let assemblyFormat = "`(` $value `)` attr-dict `:` type($res)";

    let builders = [
        OpBuilder<(ins "mlir::TypedAttr":$value), [{
            build($_builder, $_state, value.getType(), value);
        }]>
    ];

    let extraClassDeclaration = [{
        template<class T> T getValueAs() { return mlir::cast<T>(getValue()); }
    }];

    let hasVerifier = 1;
    let hasFolder = 1;
}

def BinOpKind_Mul    : I32EnumAttrCase<"Mul",   1, "mul">;
def BinOpKind_Div    : I32EnumAttrCase<"Div",   2, "div">;
def BinOpKind_Mod    : I32EnumAttrCase<"Mod",   3, "mod">;
def BinOpKind_Add    : I32EnumAttrCase<"Add",   4, "add">;
def BinOpKind_Sub    : I32EnumAttrCase<"Sub",   5, "sub">;
def BinOpKind_AddSat : I32EnumAttrCase<"AddSat",6, "sadd">;
def BinOpKind_SubSat : I32EnumAttrCase<"SubSat",7, "ssub">;
def BinOpKind_BOr    : I32EnumAttrCase<"Or",    8, "or">;
def BinOpKind_BXor   : I32EnumAttrCase<"Xor",   9, "xor">;
def BinOpKind_BAnd   : I32EnumAttrCase<"And",   10,"and">;

def BinOpKind : I32EnumAttr<"BinOpKind",
    "binary operation (arith and logic) kind",
    [BinOpKind_Mul, BinOpKind_Div, BinOpKind_Mod,
     BinOpKind_Add, BinOpKind_Sub,
     BinOpKind_AddSat, BinOpKind_SubSat,
     BinOpKind_BOr, BinOpKind_BXor, BinOpKind_BAnd]> {
    let cppNamespace = "mlir::vdpp";
}

def BinOp : vDPP_Op<"binop",
    [Pure, SameTypeOperands, SameOperandsAndResultType,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {

    let summary = "Binary operations (arith and logic)";

    let results = (outs AnyType:$result);
    let arguments = (ins Arg<BinOpKind, "binop kind">:$kind,
                        AnyType:$lhs, AnyType:$rhs);

    let assemblyFormat = [{
        `(` $kind `,` $lhs `,` $rhs  `)` `:` type($lhs) attr-dict
    }];
}

#endif // VDPPOPS