//===- vDRMTOps.td - vDRMT dialect ops -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMTOPS
#define VDRMTOPS

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

include "Dialect/vDRMT/IR/vDRMTDialect.td"
include "Dialect/vDRMT/IR/vDRMTTypes.td"

//===----------------------------------------------------------------------===//
// Base vDRMT operation definition.
//===----------------------------------------------------------------------===//

class vDRMT_Op<string mnemonic, list<Trait> traits = []> :
        Op<vDRMT_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// vDRMT operation definitions.
//===----------------------------------------------------------------------===//

def ConstantOp : vDRMT_Op<"constant",
    [ConstantLike, Pure,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Defines a vDRMT constant";

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
    let cppNamespace = "mlir::vdrmt";
}

def BinOp : vDRMT_Op<"binop",
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

def TableOp : vDRMT_Op<"table",
   [Symbol, NoTerminator]> {
  let arguments = (ins SymbolNameAttr:$sym_name, OptionalAttr<DictionaryAttr>:$annotations);
  let regions = (region SizedRegion<1>:$body);

  let builders = [
    OpBuilder<(ins "llvm::StringRef":$name, "mlir::DictionaryAttr":$annotations,
              "llvm::function_ref<void(mlir::OpBuilder &, mlir::Location)>":$entryBuilder)>,
  ];

  let assemblyFormat = [{
    $sym_name (`annotations` $annotations^)? $body attr-dict
  }];
}

def TableActionOp : vDRMT_Op<"table_action",
    [AutomaticAllocationScope, NoTerminator,
     // DeclareOpInterfaceMethods<SymbolUserOpInterface>
    ]> {
  let arguments = (ins TypeAttrOf<FuncType>:$cplaneType, SymbolRefAttr:$action,
                       OptionalAttr<DictArrayAttr>:$arg_attrs,
                       OptionalAttr<DictionaryAttr>:$annotations);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;

  let skipDefaultBuilders = 1;

  let builders = [
    OpBuilder<(ins "mlir::SymbolRefAttr":$action, "vdrmt::FuncType":$cplaneType,
                   "llvm::ArrayRef<mlir::DictionaryAttr>":$argAttrs,
                   "mlir::DictionaryAttr":$annotations,
                   "llvm::function_ref<void(mlir::OpBuilder &, mlir::Block::BlockArgListType, mlir::Location)>":$entryBuilder)>
  ];
}

#endif // VDRMTOPS