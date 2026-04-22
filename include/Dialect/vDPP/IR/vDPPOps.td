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

//===----------------------------------------------------------------------===//
// Fixed-Function Accelerator ops (vDPP-level, software-mapped)
//===----------------------------------------------------------------------===//

def vDPP_CounterInstanceOp : vDPP_Op<"counter_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a software counter array at module scope (vDPP level)";
    let description = [{
        Declares a named counter array for use in programmable blocks.
        Maps to an atomic counter array in DPA/ARM C code.
        size: number of counter cells.

        Example:
          vdpp.counter_instance @syn_count {size = 1024 : i32}
    }];
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        I32Attr:$size
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_CounterCountOp : vDPP_Op<"counter.count",
        [VFFAExecuteOpInterface]> {
    let summary = "Increment the software counter at the given index";
    let description = [{
        Increments counter instance @sym by 1 at the given index.
        Lowers to an atomic increment in DPA/ARM C code.

        Example:
          vdpp.counter.count @syn_count[%idx] : index
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        AnyInteger:$index
    );
    let assemblyFormat = "$instance `[` $index `]` `:` type($index) attr-dict";
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() { return {getIndex()}; }
        void removeHelperAttrs() { (*this)->removeAttr("vdpp.counter_size"); }
    }];
}

def vDPP_Hash5TupleInstanceOp : vDPP_Op<"hash5tuple_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a software hash-function instance at module scope (vDPP level)";
    let description = [{
        Declares a named Hash5Tuple instance backed by a software hash computation.
        Unlike the vDRMT counterpart, there is no nr_entries constraint — the SW
        implementation returns a raw hash value and the caller decides the range.
        On BF3 DPA, this lowers to a CRC or Toeplitz computation in C.

        Example:
          vdpp.hash5tuple_instance @lb_hash
    }];
    let arguments = (ins
        SymbolNameAttr:$sym_name
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_Hash5TupleApplyOp : vDPP_Op<"hash5tuple.apply",
        [VFFAExecuteOpInterface]> {
    let summary = "Compute a 5-tuple hash in software";
    let description = [{
        Computes Hash5Tuple(srcAddr, dstAddr, srcPort, dstPort, proto)
        via a software hash function, returning a 32-bit value.
        Fields may be arbitrary integers (not limited to outer packet headers).

        Example:
          %h = vdpp.hash5tuple.apply @lb_hash(%src, %dst, %sport, %dport, %proto)
                 : (i32, i32, i16, i16, i8) -> i32
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        AnyInteger:$src_addr,
        AnyInteger:$dst_addr,
        AnyInteger:$src_port,
        AnyInteger:$dst_port,
        AnyInteger:$proto
    );
    let results = (outs AnyInteger:$result);
    let assemblyFormat = [{
        $instance `(` $src_addr `,` $dst_addr `,` $src_port `,` $dst_port `,` $proto `)`
        `:` `(` type($src_addr) `,` type($dst_addr) `,` type($src_port) `,`
               type($dst_port) `,` type($proto) `)` `->` type($result) attr-dict
    }];
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() {
            return {getSrcAddr(), getDstAddr(), getSrcPort(), getDstPort(), getProto()};
        }
        void removeHelperAttrs() {}
    }];
}

def vDPP_RegisterInstanceOp : vDPP_Op<"register_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a software register array at module scope (vDPP level)";
    let description = [{
        Declares a named register array (key-value store) for use in
        programmable blocks.  On ARM, the backend is an RTE hash table
        (size configurable, default 4096).

        Example:
          vdpp.register_instance @pkt_counter {size = 1024 : i32, element_width = 32 : i32}
    }];
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        I32Attr:$size,
        I32Attr:$element_width
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_RegisterReadOp : vDPP_Op<"register.read",
        [VFFAExecuteOpInterface]> {
    let summary = "Read one element from a software register array";
    let description = [{
        Reads the value at the given index from a register instance.
        On ARM, this performs an RTE hash lookup.

        Example:
          %val = vdpp.register.read @pkt_counter[%idx] : i32 -> i32
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        AnyInteger:$index
    );
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$instance `[` $index `]` `:` type($index) `->` type($result) attr-dict";
    let extraClassDeclaration = [{
        mlir::ValueRange getInputs() { return {getIndex()}; }
        void removeHelperAttrs() {}
    }];
}

def vDPP_RegisterWriteOp : vDPP_Op<"register.write",
        [VFFAExecuteOpInterface]> {
    let summary = "Write one element into a software register array";
    let description = [{
        Writes a value at the given index into a register instance.
        On ARM, this performs an RTE hash insert/update.

        Example:
          vdpp.register.write @pkt_counter[%idx] = %val : i32, i32
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        AnyInteger:$index,
        AnyInteger:$value
    );
    let assemblyFormat = "$instance `[` $index `]` `=` $value `:` type($index) `,` type($value) attr-dict";
    let extraClassDeclaration = [{
        mlir::ValueRange getInputs() { return {getIndex(), getValue()}; }
        void removeHelperAttrs() {}
    }];
}

//===----------------------------------------------------------------------===//
// SHA / RegEx / Compression (Fixed-Function Accelerators)
//===----------------------------------------------------------------------===//
//
// These FFAs model BlueField-class accelerators reached via DOCA library
// calls (libdoca_sha, libdoca_regex, libdoca_compress) on the ARM cores.
// At the vDPP level, the execute ops are synchronous (one input → one or
// more outputs); the ARM lowering decides whether to emit synchronous or
// async DOCA submit/poll sequences.
//
// DOCA Flow (the NIC-ASIC pipeline) cannot reach these accelerators, so
// the vDRMT→BF*DRMT lowerings reject them; only the ARM-targeted
// lowerings (vDPP→LLVM) actually emit code for them.

// ── SHA ──────────────────────────────────────────────────────────────────────

def vDPP_ShaAlgo_SHA1   : I32EnumAttrCase<"SHA1",   1, "sha1">;
def vDPP_ShaAlgo_SHA256 : I32EnumAttrCase<"SHA256", 2, "sha256">;
def vDPP_ShaAlgo_SHA512 : I32EnumAttrCase<"SHA512", 3, "sha512">;

def vDPP_ShaAlgorithm : I32EnumAttr<"ShaAlgorithm",
    "SHA digest algorithm",
    [vDPP_ShaAlgo_SHA1, vDPP_ShaAlgo_SHA256, vDPP_ShaAlgo_SHA512]> {
    let cppNamespace = "mlir::vdpp";
}

def vDPP_ShaInstanceOp : vDPP_Op<"sha_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a SHA accelerator instance at module scope";
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        vDPP_ShaAlgorithm:$algorithm
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_ShaComputeOp : vDPP_Op<"sha.compute",
        [VFFAExecuteOpInterface]> {
    let summary = "Compute a SHA digest over an input buffer";
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        PointerType:$input,
        AnyInteger:$input_len,
        PointerType:$digest
    );
    let assemblyFormat = [{
        $instance `(` $input `,` $input_len `)` `->` $digest
        `:` `(` type($input) `,` type($input_len) `)` `->` type($digest) attr-dict
    }];
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() { return {getInput(), getInputLen()}; }
        void removeHelperAttrs() { (*this)->removeAttr("vdpp.sha_algorithm"); }
    }];
}

// ── RegEx ────────────────────────────────────────────────────────────────────

def vDPP_RegexInstanceOp : vDPP_Op<"regex_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a RegEx accelerator instance at module scope";
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        StrAttr:$rules
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_RegexMatchOp : vDPP_Op<"regex.match",
        [VFFAExecuteOpInterface]> {
    let summary = "Run RegEx match over an input buffer";
    let description = [{
        Returns DOCA-style (match_id, offset, length); match_id == -1 means
        no match. P4-style "matched" boolean = (match_id != -1).
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        PointerType:$input,
        AnyInteger:$input_len
    );
    let results = (outs
        AnyInteger:$match_id,
        AnyInteger:$offset,
        AnyInteger:$length
    );
    let assemblyFormat = [{
        $instance `(` $input `,` $input_len `)`
        `:` `(` type($input) `,` type($input_len) `)`
        `->` `(` type($match_id) `,` type($offset) `,` type($length) `)` attr-dict
    }];
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() { return {getInput(), getInputLen()}; }
        void removeHelperAttrs() { (*this)->removeAttr("vdpp.regex_rules"); }
    }];
}

// ── Compression ──────────────────────────────────────────────────────────────

def vDPP_CompressAlgo_DEFLATE : I32EnumAttrCase<"DEFLATE", 1, "deflate">;
def vDPP_CompressAlgo_LZ4     : I32EnumAttrCase<"LZ4",     2, "lz4">;
def vDPP_CompressAlgo_ZLIB    : I32EnumAttrCase<"ZLIB",    3, "zlib">;

def vDPP_CompressAlgorithm : I32EnumAttr<"CompressAlgorithm",
    "Compression algorithm",
    [vDPP_CompressAlgo_DEFLATE, vDPP_CompressAlgo_LZ4, vDPP_CompressAlgo_ZLIB]> {
    let cppNamespace = "mlir::vdpp";
}

def vDPP_CompressInstanceOp : vDPP_Op<"compress_instance",
        [Symbol, VFFAInstanceOpInterface]> {
    let summary = "Declare a compression accelerator instance at module scope";
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        vDPP_CompressAlgorithm:$algorithm
    );
    let assemblyFormat = "$sym_name attr-dict";
}

def vDPP_CompressOp : vDPP_Op<"compress",
        [VFFAExecuteOpInterface]> {
    let summary = "Compress an input buffer";
    let description = [{
        $output_len is a pointer to an integer slot; the verifier enforces
        this so the BF lowering can bound-check the output buffer.
    }];
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        PointerType:$input,
        AnyInteger:$input_len,
        PointerType:$output,
        PointerType:$output_len
    );
    let assemblyFormat = [{
        $instance `(` $input `,` $input_len `)` `->` `(` $output `,` $output_len `)`
        `:` `(` type($input) `,` type($input_len) `)`
        `->` `(` type($output) `,` type($output_len) `)` attr-dict
    }];
    let hasVerifier = 1;
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() { return {getInput(), getInputLen()}; }
        void removeHelperAttrs() { (*this)->removeAttr("vdpp.compress_algorithm"); }
    }];
}

def vDPP_DecompressOp : vDPP_Op<"decompress",
        [VFFAExecuteOpInterface]> {
    let summary = "Decompress an input buffer";
    let arguments = (ins
        FlatSymbolRefAttr:$instance,
        PointerType:$input,
        AnyInteger:$input_len,
        PointerType:$output,
        PointerType:$output_len
    );
    let assemblyFormat = [{
        $instance `(` $input `,` $input_len `)` `->` `(` $output `,` $output_len `)`
        `:` `(` type($input) `,` type($input_len) `)`
        `->` `(` type($output) `,` type($output_len) `)` attr-dict
    }];
    let hasVerifier = 1;
    let extraClassDeclaration = [{
        // VFFAExecuteOpInterface
        mlir::ValueRange getInputs() { return {getInput(), getInputLen()}; }
        void removeHelperAttrs() { (*this)->removeAttr("vdpp.compress_algorithm"); }
    }];
}

#endif // VDPPOPS