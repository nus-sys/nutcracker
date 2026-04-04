//===- BF3DPAOps.td - BF3 DPA cores dialect ops -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DPAOPS
#define BF3DPAOPS

include "mlir/IR/OpBase.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/MemorySlotInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/CastInterfaces.td"
include "mlir/Dialect/LLVMIR/LLVMOpBase.td"

include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.td"

//===----------------------------------------------------------------------===//
// Base BF3DPA operation definition.
//===----------------------------------------------------------------------===//

class BF3DPA_Op<string mnemonic, list<Trait> traits = []> :
        Op<BF3DPA_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// Enumerations
//===----------------------------------------------------------------------===//

// Memory fence direction (matches __DPA_R, __DPA_W, __DPA_RW semantics).
def BF3DPAFenceDir_R  : I32EnumAttrCase<"R",  0, "r">;
def BF3DPAFenceDir_W  : I32EnumAttrCase<"W",  1, "w">;
def BF3DPAFenceDir_RW : I32EnumAttrCase<"RW", 2, "rw">;

def BF3DPAFenceDir : I32EnumAttr<"BF3DPAFenceDir",
    "DPA memory fence direction",
    [BF3DPAFenceDir_R, BF3DPAFenceDir_W, BF3DPAFenceDir_RW]> {
    let cppNamespace = "::mlir::bf3dpa";
}

// ICmpPredicate mirrors vDPP (same hardware semantics, ARM integer comparisons).
def BF3DPAICmpPredicateAttr : I64EnumAttr<
    "BF3DPAICmpPredicate",
    "BF3 DPA integer comparison predicate",
    [
        I64EnumAttrCase<"eq",  0, "eq">,
        I64EnumAttrCase<"ne",  1, "ne">,
        I64EnumAttrCase<"slt", 2, "slt">,
        I64EnumAttrCase<"sle", 3, "sle">,
        I64EnumAttrCase<"sgt", 4, "sgt">,
        I64EnumAttrCase<"sge", 5, "sge">,
        I64EnumAttrCase<"ult", 6, "ult">,
        I64EnumAttrCase<"ule", 7, "ule">,
        I64EnumAttrCase<"ugt", 8, "ugt">,
        I64EnumAttrCase<"uge", 9, "uge">
    ]> {
    let cppNamespace = "::mlir::bf3dpa";
}

//===----------------------------------------------------------------------===//
// Memory Partition Annotation Ops
//
// These two ops are the mechanism for expressing and verifying the DPA memory
// partitioning constraint. They do not generate hardware instructions —
// they annotate the IR for the verifier.
//===----------------------------------------------------------------------===//

def BF3DPA_ProvisionedPtrOp : BF3DPA_Op<"provisioned_ptr", [Pure, SameOperandsAndResultType]> {
    let summary = "Mark a pointer as originating from control-plane provisioned memory";
    let description = [{
        Annotates a pointer value as a control-plane provisioned memory base.
        DPA memory buffers (WQ rings, CQ rings, TX buffers, etc.) are allocated
        by the host control plane and passed to the DPA event handler as opaque
        addresses.

        Any bf3dpa.load or bf3dpa.store accessing memory derived from a
        provisioned_ptr must use only packet_index-derived GEP indices, satisfying
        the partitioning constraint: address = provisioned_base + f(packet_fields).

        Typically wraps function arguments that represent pre-allocated rings.

        Example:
        ```mlir
        // %arg0 is the wqd_daddr passed from the host
        %ring_base = bf3dpa.provisioned_ptr %arg0 : !bf3dpa.ptr<i8>
        %slot_ptr = bf3dpa.getelementptr %ring_base[%pkt_idx] : ...
        %data = bf3dpa.load %slot_ptr : ...   // valid: index is packet-derived
        ```
    }];
    let arguments = (ins AnyType:$input);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        $input `:` type($input) attr-dict
    }];
    let hasVerifier = 1;
}

def BF3DPA_PacketIndexOp : BF3DPA_Op<"packet_index", [Pure, SameOperandsAndResultType]> {
    let summary = "Mark an integer as derived from packet header fields";
    let description = [{
        Annotates an integer value as being derived from packet header fields
        (e.g., five-tuple hash result, header field value, WQE counter from CQE).

        This annotation is required for GEP indices into provisioned memory.
        The verifier uses this to confirm the partitioning constraint:
        every non-local memory access uses a packet-derived index.

        Example:
        ```mlir
        // wqe_counter extracted from CQE is packet-derived
        %wqe_idx = bf3dpa.packet_index %cqe_wqe_counter : i32
        %slot_ptr = bf3dpa.getelementptr %ring_base[%wqe_idx] : ...
        ```
    }];
    let arguments = (ins AnyInteger:$input);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = [{
        $input `:` type($input) attr-dict
    }];
}

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

def BF3DPA_ConstantOp : BF3DPA_Op<"constant",
    [ConstantLike, Pure,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Defines a bf3dpa constant";
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
// Arithmetic Operations (all standard ops — DPA ARM cores support bitwise)
//===----------------------------------------------------------------------===//

def BF3DPA_AddOp : BF3DPA_Op<"add", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Integer addition";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_SubOp : BF3DPA_Op<"sub", [Pure, SameOperandsAndResultType]> {
    let summary = "Integer subtraction";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_MulOp : BF3DPA_Op<"mul", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Integer multiplication";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_DivOp : BF3DPA_Op<"div", [Pure, SameOperandsAndResultType]> {
    let summary = "Signed integer division";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_UDivOp : BF3DPA_Op<"udiv", [Pure, SameOperandsAndResultType]> {
    let summary = "Unsigned integer division";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// Bitwise Operations (fully supported on DPA ARM cores)
//===----------------------------------------------------------------------===//

def BF3DPA_AndOp : BF3DPA_Op<"and", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise AND";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_OrOp : BF3DPA_Op<"or", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise OR";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_XorOp : BF3DPA_Op<"xor", [Pure, Commutative, SameOperandsAndResultType]> {
    let summary = "Bitwise XOR";
    let arguments = (ins AnyInteger:$lhs, AnyInteger:$rhs);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def BF3DPA_ShlOp : BF3DPA_Op<"shl", [Pure]> {
    let summary = "Shift left";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

def BF3DPA_LShrOp : BF3DPA_Op<"lshr", [Pure]> {
    let summary = "Logical shift right";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

def BF3DPA_AShrOp : BF3DPA_Op<"ashr", [Pure]> {
    let summary = "Arithmetic shift right";
    let arguments = (ins AnyInteger:$value, AnyInteger:$amount);
    let results = (outs AnyInteger:$result);
    let assemblyFormat = "$value `,` $amount attr-dict `:` type($value) `,` type($amount) `->` type($result)";
}

//===----------------------------------------------------------------------===//
// Comparison Operations
//===----------------------------------------------------------------------===//

def BF3DPA_ICmpOp : BF3DPA_Op<"icmp", [Pure, SameTypeOperands]> {
    let summary = "Integer comparison";
    let arguments = (ins
        BF3DPAICmpPredicateAttr:$predicate,
        AnyInteger:$lhs,
        AnyInteger:$rhs
    );
    let results = (outs I1:$result);
    let assemblyFormat = "$predicate $lhs `,` $rhs attr-dict `:` type($lhs)";
}

//===----------------------------------------------------------------------===//
// Memory Operations
//
// Verifier constraint on load and store:
//   If the pointer traces back to a bf3dpa.provisioned_ptr (through any chain
//   of bf3dpa.getelementptr), then all GEP indices along that chain must be
//   provably derived from bf3dpa.packet_index ops.
//   Pointers from bf3dpa.alloca (stack) have no such constraint.
//===----------------------------------------------------------------------===//

def BF3DPA_AllocaOp : BF3DPA_Op<"alloca", [
    DeclareOpInterfaceMethods<PromotableAllocationOpInterface>
]> {
    let summary = "Thread-local stack allocation";
    let description = [{
        Allocates memory on the DPA thread's local stack. No partitioning
        constraint applies — stack memory is private to the thread.
    }];
    let arguments = (ins
        Optional<AnyInteger>:$arraySize,
        DefaultValuedAttr<I64Attr, "1">:$alignment
    );
    let results = (outs AnyType:$result);
    let assemblyFormat = "(`array_size` `=` $arraySize^ `:` type($arraySize))? attr-dict `:` type($result)";
    let builders = [
        OpBuilder<(ins "mlir::Type":$elementType), [{
            // Element type is carried in the pointer type (vDPP PointerType)
            build($_builder, $_state, elementType, Value(),
                  $_builder.getI64IntegerAttr(1));
        }]>
    ];
}

def BF3DPA_LoadOp : BF3DPA_Op<"load", []> {
    let summary = "Load from memory with partitioning verification";
    let description = [{
        Loads a value from memory. If the pointer originates from a
        bf3dpa.provisioned_ptr, the verifier ensures all GEP indices along
        the address computation chain are packet_index-derived.
    }];
    let arguments = (ins
        AnyType:$ptr,
        DefaultValuedAttr<I64Attr, "0">:$alignment
    );
    let results = (outs AnyType:$result);
    let assemblyFormat = "$ptr attr-dict `:` type($ptr) `->` type($result)";
    let hasVerifier = 1;
}

def BF3DPA_StoreOp : BF3DPA_Op<"store", []> {
    let summary = "Store to memory with partitioning verification";
    let description = [{
        Stores a value to memory. Same partitioning constraint as load:
        if the pointer comes from a provisioned_ptr, the GEP index must
        be packet_index-derived.
    }];
    let arguments = (ins
        AnyType:$value,
        AnyType:$ptr,
        DefaultValuedAttr<I64Attr, "0">:$alignment
    );
    let assemblyFormat = "$value `,` $ptr attr-dict `:` type($value) `,` type($ptr)";
    let hasVerifier = 1;
}

def BF3DPA_GetElementPtrOp : BF3DPA_Op<"getelementptr", [Pure]> {
    let summary = "Compute address of a subelement";
    let description = [{
        Computes the address of an element. When the base pointer comes from
        a provisioned_ptr, indices must be packet_index-derived to satisfy
        the memory partitioning constraint.
    }];
    let arguments = (ins
        AnyType:$base,
        Variadic<AnyInteger>:$indices
    );
    let results = (outs AnyType:$result);
    let assemblyFormat = "$base `[` $indices `]` attr-dict `:` type($base) `,` type($indices) `->` type($result)";
}

//===----------------------------------------------------------------------===//
// DPA-Specific Intrinsics
//===----------------------------------------------------------------------===//

def BF3DPA_ThreadFenceOp : BF3DPA_Op<"thread_fence"> {
    let summary = "DPA memory ordering fence";
    let description = [{
        Issues a __dpa_thread_fence(__DPA_MEMORY, dir, dir) intrinsic.
        Required for memory ordering around queue accesses and doorbells.

        dir = r  : order preceding reads  (DPA_R)
        dir = w  : order preceding writes (DPA_W)
        dir = rw : order both             (DPA_RW)

        Example:
        ```mlir
        bf3dpa.thread_fence(rw)   // __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W)
        ```
    }];
    let arguments = (ins BF3DPAFenceDir:$dir);
    let assemblyFormat = "`(` $dir `)` attr-dict";
}

def BF3DPA_ThreadIdOp : BF3DPA_Op<"thread_id", [Pure]> {
    let summary = "Get the DPA thread ID";
    let description = [{
        Returns the current DPA thread ID. Useful as a packet_index-derivable
        value when combined with packet header fields for address computation.
    }];
    let results = (outs I32:$result);
    let assemblyFormat = "attr-dict";
}

//===----------------------------------------------------------------------===//
// Control Flow Operations
//===----------------------------------------------------------------------===//

def BF3DPA_BranchOp : BF3DPA_Op<"br", [Terminator]> {
    let summary = "Unconditional branch";
    let arguments = (ins Variadic<AnyType>:$destOperands);
    let successors = (successor AnySuccessor:$dest);
    let assemblyFormat = "$dest (`(` $destOperands^ `:` type($destOperands) `)`)? attr-dict";
    let builders = [
        OpBuilder<(ins "mlir::Block *":$dest, CArg<"mlir::ValueRange", "{}">:$destOperands), [{
            $_state.addSuccessors(dest);
            $_state.addOperands(destOperands);
        }]>
    ];
}

def BF3DPA_CondBranchOp : BF3DPA_Op<"cond_br", [
    Terminator,
    AttrSizedOperandSegments,
    DeclareOpInterfaceMethods<BranchOpInterface>
]> {
    let summary = "Conditional branch";
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
        OpBuilder<(ins "mlir::Value":$condition, "mlir::Block *":$trueDest, "mlir::Block *":$falseDest,
                       CArg<"mlir::ValueRange", "{}">:$trueOperands,
                       CArg<"mlir::ValueRange", "{}">:$falseOperands), [{
            $_state.addOperands(condition);
            $_state.addOperands(trueOperands);
            $_state.addOperands(falseOperands);
            $_state.addSuccessors(trueDest);
            $_state.addSuccessors(falseDest);
        }]>
    ];
}

//===----------------------------------------------------------------------===//
// Type Conversion
//===----------------------------------------------------------------------===//

def BF3DPA_CastOp : BF3DPA_Op<"cast", [Pure]> {
    let summary = "Type conversion/cast";
    let arguments = (ins AnyType:$value);
    let results = (outs AnyType:$result);
    let assemblyFormat = "$value attr-dict `:` type($value) `to` type($result)";
}

//===----------------------------------------------------------------------===//
// Return
//===----------------------------------------------------------------------===//

def BF3DPA_ReturnOp : BF3DPA_Op<"return", [Pure, Terminator, ReturnLike]> {
    let summary = "Return from DPA event handler function";
    let description = [{
        Returns from a DPA function. The optional successor attribute carries
        the block scheduling hint (vDSA block succession), same semantics as
        vdpp.return.
    }];
    let arguments = (ins
        Variadic<AnyType>:$operands,
        OptionalAttr<I32Attr>:$successor
    );
    let assemblyFormat = "($operands^ `:` type($operands))? attr-dict";
    let builders = [
        OpBuilder<(ins), [{
            build($_builder, $_state, std::nullopt, nullptr);
        }]>,
        OpBuilder<(ins "int32_t":$successorId), [{
            build($_builder, $_state, std::nullopt,
                  $_builder.getI32IntegerAttr(successorId));
        }]>
    ];
}

//===----------------------------------------------------------------------===//
// Hash5Tuple — software 5-tuple hash on BF3 DPA
//===----------------------------------------------------------------------===//

def BF3DPA_Hash5TupleInstanceOp : BF3DPA_Op<"hash5tuple_instance", [Symbol]> {
    let summary = "Declare a software Hash5Tuple instance at module scope (BF3 DPA)";
    let description = [{
        Declares a named Hash5Tuple instance backed by a software hash computation.
        On BF3 DPA, this lowers to a CRC32 or Toeplitz hash function call in C.
        Unlike the vDRMT counterpart, there is no nr_entries constraint.

        Example:
          bf3dpa.hash5tuple_instance @lb_hash
    }];
    let arguments = (ins SymbolNameAttr:$sym_name);
    let assemblyFormat = "$sym_name attr-dict";
}

def BF3DPA_Hash5TupleApplyOp : BF3DPA_Op<"hash5tuple.apply"> {
    let summary = "Compute a 5-tuple hash on BF3 DPA (software CRC/Toeplitz)";
    let description = [{
        Computes Hash5Tuple(srcAddr, dstAddr, srcPort, dstPort, proto)
        via a software hash function on the DPA, returning a 32-bit value.
        The caller is responsible for applying a modulo or mask to obtain a
        bucket index.

        Example:
          %h = bf3dpa.hash5tuple.apply @lb_hash(%src, %dst, %sport, %dport, %proto)
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
}

#endif // BF3DPAOPS
