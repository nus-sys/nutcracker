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

// ============================================================================
// Constants and Variables
// ============================================================================

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

def VariableOp : vDRMT_Op<"variable", [Pure]> {
    let summary = "Declares a local variable";
    let description = [{
        Creates a reference to a local variable that can be read/written.
        Example: %var = vdrmt.variable ["temp", init] : <!vdrmt.ref<i32>>
    }];
    
    let arguments = (ins StrAttr:$name, OptionalAttr<UnitAttr>:$init);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        `[` $name (`,` `init` $init^)? `]` attr-dict `:` type($result)
    }];
}

// ============================================================================
// Memory Operations
// ============================================================================

def ReadOp : vDRMT_Op<"read", [
    Pure,
    TypesMatchWith<"type of 'result' matches object type of 'ref'",
                   "ref", "result",
                   "mlir::cast<vdrmt::ReferenceType>($_self).getObjectType()">,
    DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    
    let summary = "Read value from variable";
    let arguments = (ins ReferenceType:$ref);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        $ref `:` type($ref) attr-dict
    }];
}

def AssignOp : vDRMT_Op<"assign"> {
    let summary = "Assign value to reference";
    let arguments = (ins AnyType:$value, ReferenceType:$ref);
    
    let assemblyFormat = [{
        $value `,` $ref attr-dict `:` type($value) `,` type($ref)
    }];
}

// ============================================================================
// Struct/Header Operations
// ============================================================================

def StructExtractOp : vDRMT_Op<"struct_extract", [Pure]> {
    let summary = "Extract field from struct by value";
    let description = [{
        Extracts a field from a struct value.
        Example: %field = vdrmt.struct_extract %struct[0] : !vdrmt.struct<...> -> i32
    }];
    
    // Avoid using C++ reserved identifier name 'struct' for the
    // argument name because TableGen uses this as a C++ parameter
    // identifier when generating headers. Use '$input' instead.
    let arguments = (ins AnyType:$input, I32Attr:$fieldIndex);
    let results = (outs AnyType:$result);

    let assemblyFormat = [{
        $input `[` $fieldIndex `]` `:` type($input) `->` type($result) attr-dict
    }];
}

def StructExtractRefOp : vDRMT_Op<"struct_extract_ref", [Pure,
    DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {

    let summary = "Extract field reference from struct";
    
    let arguments = (ins ReferenceType:$input, I32Attr:$fieldIndex);
    let results = (outs ReferenceType:$result);

    let hasCustomAssemblyFormat = 1;
    let hasVerifier = 1;

    let builders = [
        OpBuilder<(ins "mlir::Value":$input, "vdrmt::FieldInfo":$field)>,
        OpBuilder<(ins "mlir::Value":$input, "mlir::StringAttr":$fieldName)>,
        OpBuilder<(ins "mlir::Value":$input, "llvm::StringRef":$fieldName), [{
            build($_builder, $_state, input, $_builder.getStringAttr(fieldName));
        }]>
    ];

    let extraClassDeclaration = [{
        mlir::StringAttr getFieldNameAttr() {
            auto refType = mlir::cast<vdrmt::ReferenceType>(getInput().getType());
            auto objectType = refType.getObjectType();
            auto structLike = mlir::cast<vDRMT_StructLikeTypeInterface>(objectType);
            return structLike.getFields()[getFieldIndex()].name;
        }

        llvm::StringRef getFieldName() {
            return getFieldNameAttr().getValue();
        }
    }];
}

// ============================================================================
// Arithmetic and Bitwise Operations
// ============================================================================

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


// ============================================================================
// Bit Manipulation Operations
// ============================================================================

def SliceOp : vDRMT_Op<"slice", [Pure]> {
    let summary = "Extract bit slice";
    let description = [{
        Extracts a contiguous range of bits from a value.
        Example: %lower = vdrmt.slice %value[15:0] : i32 -> i16
    }];
    
    let arguments = (ins AnyType:$input, I32Attr:$highBit, I32Attr:$lowBit);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        $input `[` $highBit `:` $lowBit `]` attr-dict `:` type($input) `->` type($result)
    }];
}

def ConcatOp : vDRMT_Op<"concat", [Pure]> {
    let summary = "Concatenate bit vectors";
    let description = [{
        Concatenates two bit vectors into a wider result.
        Example: %wide = vdrmt.concat(%upper : i16, %lower : i16) : i32
        Result is [upper : lower]
    }];
    
    let arguments = (ins AnyType:$upper, AnyType:$lower);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        `(` $upper `:` type($upper) `,` $lower `:` type($lower) `)` `:` type($result) attr-dict
    }];
    
    let builders = [
        OpBuilder<(ins "mlir::Value":$upper, "mlir::Value":$lower, "mlir::Type":$resultType), [{
            build($_builder, $_state, resultType, upper, lower);
        }]>
    ];
}

// ============================================================================
// Shift Operations
// ============================================================================

def ShlOp : vDRMT_Op<"shl", 
    [Pure,
     TypesMatchWith<
       "result type must equal lhs type",
       "result", "lhs", "$_self">]> {
    let summary = "Shift left";
    let description = [{
        Logical shift left operation.
        Example: %result = vdrmt.shl(%value, %amount : i32) : i32
    }];
    
    let arguments = (ins AnyType:$lhs, AnyType:$rhs);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        `(` $lhs `,` $rhs `:` type($rhs) `)` `:` type($result) attr-dict
    }];
    
    let builders = [
        OpBuilder<(ins "mlir::Value":$lhs, "mlir::Value":$rhs), [{
            build($_builder, $_state, lhs.getType(), lhs, rhs);
        }]>
    ];
}

def ShrOp : vDRMT_Op<"shr",
    [Pure,
     TypesMatchWith<
       "result type must equal lhs type",
       "result", "lhs", "$_self">]> {
    let summary = "Shift right";
    let description = [{
        Logical shift right operation.
        Example: %result = vdrmt.shr(%value, %amount : i32) : i32
    }];
    
    let arguments = (ins AnyType:$lhs, AnyType:$rhs);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = [{
        `(` $lhs `,` $rhs `:` type($rhs) `)` `:` type($result) attr-dict
    }];
    
    let builders = [
        OpBuilder<(ins "mlir::Value":$lhs, "mlir::Value":$rhs), [{
            build($_builder, $_state, lhs.getType(), lhs, rhs);
        }]>
    ];
}

// ============================================================================
// Type Conversion
// ============================================================================

def CastOp : vDRMT_Op<"cast", [Pure]> {
    let arguments = (ins AnyType:$value);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        `(` $value `:` type($value) `)` `:` type($result) attr-dict
    }];
}

// ============================================================================
// Comparison Operations
// ============================================================================

def CmpOpKind_Eq  : I32EnumAttrCase<"Eq",  0, "eq">;
def CmpOpKind_Ne  : I32EnumAttrCase<"Ne",  1, "ne">;
def CmpOpKind_Lt  : I32EnumAttrCase<"Lt",  2, "lt">;
def CmpOpKind_Le  : I32EnumAttrCase<"Le",  3, "le">;
def CmpOpKind_Gt  : I32EnumAttrCase<"Gt",  4, "gt">;
def CmpOpKind_Ge  : I32EnumAttrCase<"Ge",  5, "ge">;

def CmpOpKind : I32EnumAttr<"CmpOpKind",
    "comparison operation kind",
    [CmpOpKind_Eq, CmpOpKind_Ne, CmpOpKind_Lt,
     CmpOpKind_Le, CmpOpKind_Gt, CmpOpKind_Ge]> {
    let cppNamespace = "mlir::vdrmt";
}

def CmpOp : vDRMT_Op<"cmp", [Pure, SameTypeOperands]> {
    let summary = "Comparison operation";
    let description = [{
        Compares two values and returns a boolean result.
        Example: %cond = vdrmt.cmp %a, %b {kind = 5 : i32} : i8
    }];
    
    let arguments = (ins AnyType:$lhs, AnyType:$rhs, I32Attr:$kind);
    let results = (outs I1:$result);
    
    let assemblyFormat = [{
        $lhs `,` $rhs `{` `kind` `=` $kind `}` attr-dict `:` type($lhs)
    }];
}

// ============================================================================
// Control Flow
// ============================================================================

def IfOp : vDRMT_Op<"if", [
    NoTerminator, 
    RecursiveMemoryEffects,
    DeclareOpInterfaceMethods<RegionBranchOpInterface>  // ADD THIS!
]> {
    let summary = "Conditional branch";
    let description = [{
        Conditional execution with optional else branch.
        Example:
          vdrmt.if %cond {
            ...
            vdrmt.yield
          } else {
            ...
            vdrmt.yield
          }
    }];
    
    let arguments = (ins I1:$condition);
    let regions = (region SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion);
    
    let skipDefaultBuilders = 1;
    let builders = [
        // Simple builder: auto-add terminators
        OpBuilder<(ins "mlir::Value":$condition, "bool":$withElseRegion)>,
        
        // Advanced builder: custom callbacks (ADD THIS!)
        OpBuilder<(ins "mlir::Value":$condition, "bool":$withElseRegion,
                       "function_ref<void(OpBuilder &, Location)>":$thenBuilder,
                       "function_ref<void(OpBuilder &, Location)>":$elseBuilder)>
    ];
    
    let hasCustomAssemblyFormat = 1;
}

def YieldOp : vDRMT_Op<"yield", [
    Pure,
    ReturnLike,
    Terminator,
    ParentOneOf<["IfOp", "TableActionOp"]>
]> {
    let summary = "Represents the default branching behaviour of a region";
    let description = [{
        The `vdrmt.yield` operation terminates regions on different vDRMT operations,
        and it is used to represent the default branching behaviour of a region.
        Said branching behaviour is determined by the parent operation. For
        example, a yield in a `vdrmt.if` region implies a branch to the exit block.

        In some cases, it might yield an SSA value and the semantics of how the
        values are yielded is defined by the parent operation.

        As a general rule, `vdrmt.yield` must be explicitly used whenever a region has
        more than one block and no terminator.

        Examples:
        ```mlir
        vdrmt.if %cond {
          ...
          vdrmt.yield
        }

        vdrmt.table_action @my_action : (i32, i16) -> () {
        ^bb0(%arg0: i32, %arg1: i16):
          ...
          vdrmt.yield
        }

        // Future: yield with values
        %result = vdrmt.some_op {
          ...
          vdrmt.yield %val : i32
        }
        ```
    }];

    let arguments = (ins Variadic<AnyType>:$args);
    let assemblyFormat = "($args^ `:` type($args))? attr-dict";

    let builders = [
        OpBuilder<(ins), [{ /* nothing to do */ }]>,
    ];
}

def NextOp : vDRMT_Op<"next", [Pure]> {
    let summary = "Chain to the next vDSA block; replaces successor attrs on return/if";
    let description = [{
        Unconditional: `vdrmt.next 1`         — always proceed to block 1.
        Conditional:   `vdrmt.next %cond, 1, 2` — proceed to 1 if %cond is true,
                                                   2 otherwise.

        `vdrmt.next` is a routing annotation, NOT an MLIR-level block terminator.
        It appears immediately before `vdrmt.return` (or `func.return`) in the
        function body, and as the last op in `vdrmt.if` then/else regions
        (which carry `NoTerminator`). The actual MLIR terminator of the function
        block is always `vdrmt.return` / `func.return`.

        This design avoids the two-terminator problem that would arise if
        `vdrmt.next` also carried the `Terminator` trait alongside `func.return`.
    }];

    let arguments = (ins
        Optional<I1>:$condition,
        I32Attr:$then_successor,
        OptionalAttr<I32Attr>:$else_successor
    );

    let assemblyFormat = [{
        ($condition^ `,`)? $then_successor (`,` $else_successor^)? attr-dict
    }];

    let hasVerifier = 1;

    let builders = [
        // Unconditional form
        OpBuilder<(ins "int32_t":$successor), [{
            build($_builder, $_state, /*condition=*/mlir::Value{},
                  $_builder.getI32IntegerAttr(successor), nullptr);
        }]>,
        // Conditional form
        OpBuilder<(ins "mlir::Value":$condition,
                       "int32_t":$thenSuccessor, "int32_t":$elseSuccessor), [{
            build($_builder, $_state, condition,
                  $_builder.getI32IntegerAttr(thenSuccessor),
                  $_builder.getI32IntegerAttr(elseSuccessor));
        }]>
    ];
}

// ============================================================================
// Table Operations
// ============================================================================

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
    [AutomaticAllocationScope, NoTerminator]> {
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

def ReturnOp : vDRMT_Op<"return", [Pure, Terminator, ReturnLike]> {
    let summary = "Return from function (use vdrmt.next to specify block successor)";
    let description = [{
        Returns from a function, optionally yielding values.
        Block successor routing is expressed via a preceding `vdrmt.next` op,
        not via attributes on return.

        Example:
        ```mlir
        vdrmt.next 1
        vdrmt.return
        ```
    }];

    let arguments = (ins Variadic<AnyType>:$operands);

    let assemblyFormat = "($operands^ `:` type($operands))? attr-dict";

    let builders = [
        OpBuilder<(ins), [{
            build($_builder, $_state, std::nullopt);
        }]>
    ];
}

#endif // VDRMTOPS