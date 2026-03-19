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

//===----------------------------------------------------------------------===//
// Arithmetic Operations
//===----------------------------------------------------------------------===//

def AddOp : vDPP_Op<"add", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Integer addition";
    let description = [{
        Adds two integer values. Similar to LLVM's add instruction.
        
        Example:
        ```mlir
        %result = vdpp.add %a, %b : i32
        ```
    }];
    
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def SubOp : vDPP_Op<"sub", [Pure, SameOperandsAndResultType]> {
    let summary = "Integer subtraction";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def MulOp : vDPP_Op<"mul", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Integer multiplication";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def DivOp : vDPP_Op<"div", [Pure, SameOperandsAndResultType]> {
    let summary = "Signed integer division";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def UDivOp : vDPP_Op<"udiv", [Pure, SameOperandsAndResultType]> {
    let summary = "Unsigned integer division";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// Bitwise Operations
//===----------------------------------------------------------------------===//

def AndOp : vDPP_Op<"and", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise AND";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def OrOp : vDPP_Op<"or", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise OR";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def XorOp : vDPP_Op<"xor", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise XOR";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def ShlOp : vDPP_Op<"shl", [Pure]> {
    let summary = "Shift left";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

def LShrOp : vDPP_Op<"lshr", [Pure]> {
    let summary = "Logical shift right";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

def AShrOp : vDPP_Op<"ashr", [Pure]> {
    let summary = "Arithmetic shift right";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

//===----------------------------------------------------------------------===//
// Comparison Operations
//===----------------------------------------------------------------------===//

def ICmpPredicateAttr : I64EnumAttr<
    "ICmpPredicate",
    "vDPP integer comparison predicate",
    [
        I64EnumAttrCase<"eq", 0, "eq">,    // equal
        I64EnumAttrCase<"ne", 1, "ne">,    // not equal
        I64EnumAttrCase<"slt", 2, "slt">,  // signed less than
        I64EnumAttrCase<"sle", 3, "sle">,  // signed less or equal
        I64EnumAttrCase<"sgt", 4, "sgt">,  // signed greater than
        I64EnumAttrCase<"sge", 5, "sge">,  // signed greater or equal
        I64EnumAttrCase<"ult", 6, "ult">,  // unsigned less than
        I64EnumAttrCase<"ule", 7, "ule">,  // unsigned less or equal
        I64EnumAttrCase<"ugt", 8, "ugt">,  // unsigned greater than
        I64EnumAttrCase<"uge", 9, "uge">   // unsigned greater or equal
    ]> {
    let cppNamespace = "::mlir::vdpp";
}

def ICmpOp : vDPP_Op<"icmp", [Pure, SameTypeOperands]> {
    let summary = "Integer comparison";
    let description = [{
        Compares two integer values using the specified predicate.
        Similar to LLVM's icmp instruction.
        
        Example:
        ```mlir
        %result = vdpp.icmp eq %a, %b : i32
        ```
    }];
    
    let arguments = (ins 
        ICmpPredicateAttr:$predicate,
        AnyInteger:$lhs,
        AnyInteger:$rhs
    );
    let results = (outs I1:$result);
    
    let assemblyFormat = "$predicate $lhs `,` $rhs attr-dict `:` type($lhs)";
}

//===----------------------------------------------------------------------===//
// Memory Operations (LLVM-style)
//===----------------------------------------------------------------------===//

def AllocaOp : vDPP_Op<"alloca", [
    DeclareOpInterfaceMethods<PromotableAllocationOpInterface>
]> {
    let summary = "Stack allocation";
    let description = [{
        Allocates memory on the stack. Similar to LLVM's alloca.
        
        Example:
        ```mlir
        %ptr = vdpp.alloca : !vdpp.ptr<i32>
        ```
    }];
    
    let arguments = (ins 
        Optional<AnyInteger>:$arraySize,
        DefaultValuedAttr<I64Attr, "1">:$alignment
    );
    let results = (outs PointerType:$result);
    
    let assemblyFormat = "(`array_size` `=` $arraySize^ `:` type($arraySize))? attr-dict `:` type($result)";
    
    let builders = [
        OpBuilder<(ins "Type":$elementType), [{
            auto ptrType = vdpp::PointerType::get($_builder.getContext(), elementType);
            build($_builder, $_state, ptrType, Value(), 
                  $_builder.getI64IntegerAttr(1));
        }]>
    ];
}

def LoadOp : vDPP_Op<"load", []> {
    let summary = "Load from memory";
    let description = [{
        Loads a value from memory. Similar to LLVM's load.
        
        Example:
        ```mlir
        %value = vdpp.load %ptr : !vdpp.ptr<i32> -> i32
        ```
    }];
    
    let arguments = (ins 
        PointerType:$ptr,
        DefaultValuedAttr<I64Attr, "0">:$alignment
    );
    let results = (outs AnyType:$result);
    
    let assemblyFormat = "$ptr attr-dict `:` type($ptr) `->` type($result)";
}

def StoreOp : vDPP_Op<"store", []> {
    let summary = "Store to memory";
    let description = [{
        Stores a value to memory. Similar to LLVM's store.
        
        Example:
        ```mlir
        vdpp.store %value, %ptr : i32, !vdpp.ptr<i32>
        ```
    }];
    
    let arguments = (ins 
        AnyType:$value,
        PointerType:$ptr,
        DefaultValuedAttr<I64Attr, "0">:$alignment
    );
    
    let assemblyFormat = "$value `,` $ptr attr-dict `:` type($value) `,` type($ptr)";
}

def GetElementPtrOp : vDPP_Op<"getelementptr", [Pure]> {
    let summary = "Get element pointer";
    let description = [{
        Computes the address of a subelement. Similar to LLVM's getelementptr.
        
        Example:
        ```mlir
        %ptr = vdpp.getelementptr %base[%index] : !vdpp.ptr<i32>, i32
        ```
    }];
    
    let arguments = (ins 
        PointerType:$base,
        Variadic<AnyInteger>:$indices
    );
    let results = (outs PointerType:$result);

    let assemblyFormat = "$base `[` $indices `]` attr-dict `:` type($base) `,` type($indices) `->` type($result)";
}

//===----------------------------------------------------------------------===//
// Control Flow Operations
//===----------------------------------------------------------------------===//

def BranchOp : vDPP_Op<"br", [Terminator]> {
    let summary = "Unconditional branch";
    let description = [{
        Branches unconditionally to another block. Similar to LLVM's br.
        
        Example:
        ```mlir
        vdpp.br ^bb1(%arg : i32)
        ```
    }];
    
    let arguments = (ins Variadic<AnyType>:$destOperands);
    let successors = (successor AnySuccessor:$dest);
    
    let assemblyFormat = "$dest (`(` $destOperands^ `:` type($destOperands) `)`)? attr-dict";
    
    let builders = [
        OpBuilder<(ins "Block *":$dest, CArg<"ValueRange", "{}">:$destOperands), [{
            $_state.addSuccessors(dest);
            $_state.addOperands(destOperands);
        }]>
    ];
}

def CondBranchOp : vDPP_Op<"cond_br", [
    Terminator,
    AttrSizedOperandSegments,
    DeclareOpInterfaceMethods<BranchOpInterface>
]> {
    let summary = "Conditional branch";
    let description = [{
        Branches to one of two blocks based on a condition.
        Similar to LLVM's br with condition.
        
        Example:
        ```mlir
        vdpp.cond_br %cond, ^bb1, ^bb2
        ```
    }];
    
    let arguments = (ins 
        I1:$condition,
        Variadic<AnyType>:$trueDestOperands,
        Variadic<AnyType>:$falseDestOperands
    );
    let successors = (successor AnySuccessor:$trueDest, AnySuccessor:$falseDest);
    
    let assemblyFormat = [{
        $condition `,`
        $trueDest (`(` $trueDestOperands^ `:` type($trueDestOperands) `)`)? `,`
        $falseDest (`(` $falseDestOperands^ `:` type($falseDestOperands) `)`)?
        attr-dict
    }];
    
    let builders = [
        OpBuilder<(ins "Value":$condition, "Block *":$trueDest, "Block *":$falseDest,
                       CArg<"ValueRange", "{}">:$trueOperands,
                       CArg<"ValueRange", "{}">:$falseOperands), [{
            $_state.addOperands(condition);
            $_state.addOperands(trueOperands);
            $_state.addOperands(falseOperands);
            $_state.addSuccessors(trueDest);
            $_state.addSuccessors(falseDest);
            $_state.addAttribute("operandSegmentSizes",
                $_builder.getDenseI32ArrayAttr({1,
                    (int32_t)trueOperands.size(),
                    (int32_t)falseOperands.size()}));
        }]>
    ];
}

//===----------------------------------------------------------------------===//
// Conversion/Cast Operations
//===----------------------------------------------------------------------===//

def CastOp : vDPP_Op<"cast", [Pure]> {
    let summary = "Type conversion/cast operation";
    let description = [{
        Converts a value from one type to another.
        This is a generic cast operation that can handle:
        - Integer width changes (truncation/extension)
        - Pointer/integer conversions
        - Bitcasts
        
        Example:
        ```mlir
        %result = vdpp.cast %value : i64 to i32
        %ptr = vdpp.cast %int : i64 to !vdpp.ptr<i32>
        ```
    }];
    
    let arguments = (ins AnyType:$value);
    let results = (outs AnyType:$result);
    
    let assemblyFormat = "$value attr-dict `:` type($value) `to` type($result)";
}

//===----------------------------------------------------------------------===//
// Function Operations
//===----------------------------------------------------------------------===//

def ReturnOp : vDPP_Op<"return", [Pure, Terminator, ReturnLike]> {
    let summary = "Return from function";
    let description = [{
        Returns from a function with optional values and optional successor hint.
        
        The successor attribute indicates which virtual block should execute next.
        -1 means end of pipeline (no successor).
        
        Example:
        ```mlir
        vdpp.return %value : i32
        vdpp.return {successor = 1 : i32}
        vdpp.return {successor = -1 : i32}  // End of pipeline
        ```
    }];
    
    let arguments = (ins 
        Variadic<AnyType>:$operands,
        OptionalAttr<I32Attr>:$successor
    );
    
    let assemblyFormat = "($operands^ `:` type($operands))? attr-dict";
    
    let builders = [
        // No arguments, no successor
        OpBuilder<(ins), [{
            build($_builder, $_state, std::nullopt, nullptr);
        }]>,
        // With successor ID
        OpBuilder<(ins "int32_t":$successorId), [{
            build($_builder, $_state, std::nullopt, 
                  $_builder.getI32IntegerAttr(successorId));
        }]>,
        // With return values, no successor
        OpBuilder<(ins "ValueRange":$operands), [{
            build($_builder, $_state, operands, nullptr);
        }]>,
        // With return values and successor
        OpBuilder<(ins "ValueRange":$operands, "int32_t":$successorId), [{
            build($_builder, $_state, operands, 
                  $_builder.getI32IntegerAttr(successorId));
        }]>
    ];
}

#endif // VDPPOPS