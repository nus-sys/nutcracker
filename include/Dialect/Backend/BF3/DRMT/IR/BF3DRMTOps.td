//===- BF3DRMTOps.td - BF3 NIC ASIC DRMT dialect ops ------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DRMTOPS
#define BF3DRMTOPS

include "mlir/IR/OpBase.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/CastInterfaces.td"

include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.td"

//===----------------------------------------------------------------------===//
// Base BF3DRMT operation definition.
//===----------------------------------------------------------------------===//

class BF3DRMT_Op<string mnemonic, list<Trait> traits = []> :
        Op<BF3DRMT_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// Enumerations
//===----------------------------------------------------------------------===//

// BinOpKind: arithmetic + bitwise ops supported at the IR level.
// Mul and Div are absent: the egglog fine-grained lowering pass rewrites them
// to SHL/SHR before reaching this dialect.
// Sub requires a constant rhs (verifier enforced); it is implemented via
// ADD overflow on the NIC.
// Note: Or/Xor/And are valid IR nodes even if the NIC lacks native support —
// later codegen passes are responsible for further lowering them.
def BF3DRMTBinOpKind_Mod    : I32EnumAttrCase<"Mod",   3, "mod">;
def BF3DRMTBinOpKind_Add    : I32EnumAttrCase<"Add",   4, "add">;
def BF3DRMTBinOpKind_Sub    : I32EnumAttrCase<"Sub",   5, "sub">;
def BF3DRMTBinOpKind_AddSat : I32EnumAttrCase<"AddSat",6, "sadd">;
def BF3DRMTBinOpKind_SubSat : I32EnumAttrCase<"SubSat",7, "ssub">;
def BF3DRMTBinOpKind_BOr    : I32EnumAttrCase<"Or",    8, "or">;
def BF3DRMTBinOpKind_BXor   : I32EnumAttrCase<"Xor",   9, "xor">;
def BF3DRMTBinOpKind_BAnd   : I32EnumAttrCase<"And",   10,"and">;
// NOTE: Mul=1, Div=2 from vDRMT are intentionally omitted (egglog rewrites them).

def BF3DRMTBinOpKind : I32EnumAttr<"BF3DRMTBinOpKind",
    "BF3 NIC ASIC binary operation kind",
    [BF3DRMTBinOpKind_Mod, BF3DRMTBinOpKind_Add, BF3DRMTBinOpKind_Sub,
     BF3DRMTBinOpKind_AddSat, BF3DRMTBinOpKind_SubSat,
     BF3DRMTBinOpKind_BOr, BF3DRMTBinOpKind_BXor, BF3DRMTBinOpKind_BAnd]> {
    let cppNamespace = "::mlir::bf3drmt";
}

// DOCA Flow pipe type — determines the matching algorithm for a pipe.
def BF3DRMTPipeType_Basic   : I32EnumAttrCase<"Basic",   0, "basic">;
def BF3DRMTPipeType_Acl     : I32EnumAttrCase<"Acl",     1, "acl">;
def BF3DRMTPipeType_Control : I32EnumAttrCase<"Control", 2, "control">;
def BF3DRMTPipeType_Hash    : I32EnumAttrCase<"Hash",    3, "hash">;
def BF3DRMTPipeType_Lpm     : I32EnumAttrCase<"Lpm",     4, "lpm">;

def BF3DRMTPipeType : I32EnumAttr<"BF3DRMTPipeType",
    "DOCA Flow pipe type",
    [BF3DRMTPipeType_Basic, BF3DRMTPipeType_Acl,
     BF3DRMTPipeType_Control, BF3DRMTPipeType_Hash,
     BF3DRMTPipeType_Lpm]> {
    let cppNamespace = "::mlir::bf3drmt";
}

//===----------------------------------------------------------------------===//
// Constants and Variables
//===----------------------------------------------------------------------===//

def BF3DRMT_ConstantOp : BF3DRMT_Op<"constant",
    [ConstantLike, Pure,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Defines a bf3drmt constant";
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

def BF3DRMT_VariableOp : BF3DRMT_Op<"variable", [Pure]> {
    let summary = "Declares a local variable reference";
    let description = [{
        Creates a reference to a local variable that can be read/written.
        Identical semantics to vdrmt.variable.
    }];
    let arguments = (ins StrAttr:$name, OptionalAttr<UnitAttr>:$init);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        `[` $name (`,` `init` $init^)? `]` attr-dict `:` type($result)
    }];
}

//===----------------------------------------------------------------------===//
// Memory / Reference Operations
//===----------------------------------------------------------------------===//

def BF3DRMT_ReadOp : BF3DRMT_Op<"read", [
    Pure,
    DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Read value from a struct field reference";
    let description = [{
        Reads a value through a reference obtained from struct_extract_ref.

        HARDWARE CONSTRAINT: On the BF3 NIC ASIC, the only valid pattern is
            struct_extract_ref -> read
        Reading a struct by value and then extracting a field (read -> struct_extract)
        is not supported. The verifier enforces this: the operand ref must be
        produced by a bf3drmt.struct_extract_ref or bf3drmt.variable op.
    }];
    let arguments = (ins AnyType:$ref);   // must be a vdrmt::ReferenceType
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        $ref `:` type($ref) `->` type($result) attr-dict
    }];
    let hasVerifier = 1;
}

def BF3DRMT_AssignOp : BF3DRMT_Op<"assign"> {
    let summary = "Assign value to a struct field reference";
    let description = [{
        Assigns a value through a reference. Used in pipe actions to modify
        packet header fields or metadata.
    }];
    let arguments = (ins AnyType:$value, AnyType:$ref);
    let assemblyFormat = [{
        $value `,` $ref attr-dict `:` type($value) `,` type($ref)
    }];
}

//===----------------------------------------------------------------------===//
// Struct Field Reference Operations
//
// NOTE: bf3drmt.struct_extract (by-value field extraction) is intentionally
// absent. The BF3 NIC ASIC requires going through references:
//   struct_extract_ref -> read       (to get a field value)
//   struct_extract_ref -> assign     (to set a field value)
//===----------------------------------------------------------------------===//

def BF3DRMT_StructExtractRefOp : BF3DRMT_Op<"struct_extract_ref", [Pure,
    DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Get a reference to a named field in a struct/header";
    let description = [{
        Returns a reference to a named field within a struct or header.
        The result reference can then be passed to read/assign.

        This is the only supported struct field access pattern on bf3drmt.
        By-value extraction (struct_extract) is not available.

        Example:
        ```mlir
        %ip_ref = bf3drmt.struct_extract_ref %hdr_ref[0] : <!vdrmt.struct<...>>
        %proto_ref = bf3drmt.struct_extract_ref %ip_ref[2] : <!vdrmt.header<...>>
        %proto = bf3drmt.read %proto_ref : <!i8>
        ```
    }];
    let arguments = (ins AnyType:$input, I32Attr:$fieldIndex);
    let results = (outs AnyType:$result);
    let hasCustomAssemblyFormat = 1;
    let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Arithmetic Operations (no bitwise — use slice/concat instead)
//===----------------------------------------------------------------------===//

def BF3DRMT_BinOp : BF3DRMT_Op<"binop",
    [Pure, SameTypeOperands, SameOperandsAndResultType,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
    let summary = "Arithmetic binary operation (NIC ASIC: no Or/Xor/And/Mul/Div)";
    let description = [{
        Performs an arithmetic binary operation. Bitwise operations and Mul/Div
        are absent:
          - Or, Xor, And: not supported on the BF3 NIC ASIC; use slice/concat.
          - Mul, Div: rewritten to SHL/SHR by the egglog fine-grained lowering pass
            before the IR is lowered to this dialect.

        Sub requires the rhs operand to be a constant (BF3 implements Sub as
        ADD overflow with a fixed immediate; the verifier enforces this).

        Supported kinds: mod, add, sub (rhs must be constant),
                         sadd (saturating add), ssub (saturating sub).
    }];
    let results = (outs AnyType:$result);
    let arguments = (ins Arg<BF3DRMTBinOpKind, "binop kind">:$kind,
                        AnyType:$lhs, AnyType:$rhs);
    let assemblyFormat = [{
        `(` $kind `,` $lhs `,` $rhs `)` `:` type($lhs) attr-dict
    }];
    let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Bit Manipulation Operations
// These replace bitwise ops on the NIC ASIC:
//   x & 0xFF      ->  bf3drmt.slice %x[7:0]
//   x & ~0xFF     ->  bf3drmt.concat(bf3drmt.slice %x[31:8], constant(0, i8))
//   x | y         ->  bf3drmt.concat(upper_bits, lower_bits) when non-overlapping
//===----------------------------------------------------------------------===//

def BF3DRMT_SliceOp : BF3DRMT_Op<"slice", [Pure]> {
    let summary = "Extract a contiguous range of bits";
    let description = [{
        Extracts bits [highBit:lowBit] (inclusive) from input.
        Primary mechanism for bitmask operations since bitwise And is unavailable.

        Example:
        ```mlir
        // x & 0xFF  ->  x[7:0]
        %low = bf3drmt.slice %x[7:0] : i32 -> i8
        ```
    }];
    let arguments = (ins AnyType:$input, I32Attr:$highBit, I32Attr:$lowBit);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        $input `[` $highBit `:` $lowBit `]` attr-dict `:` type($input) `->` type($result)
    }];
}

def BF3DRMT_ConcatOp : BF3DRMT_Op<"concat", [Pure]> {
    let summary = "Concatenate two bit vectors";
    let description = [{
        Concatenates upper and lower bit vectors: result = [upper : lower].
        Used together with slice to implement bitmask / field assembly operations.

        Example:
        ```mlir
        // Build a 32-bit value from two 16-bit halves
        %result = bf3drmt.concat(%hi : i16, %lo : i16) : i32
        ```
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

def BF3DRMT_ShlOp : BF3DRMT_Op<"shl", [Pure]> {
    let summary = "Shift left";
    let arguments = (ins AnyType:$lhs, AnyType:$rhs);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        `(` $lhs `:` type($lhs) `,` $rhs `:` type($rhs) `)` `:` type($result) attr-dict
    }];
}

def BF3DRMT_ShrOp : BF3DRMT_Op<"shr", [Pure]> {
    let summary = "Shift right (logical)";
    let arguments = (ins AnyType:$lhs, AnyType:$rhs);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        `(` $lhs `:` type($lhs) `,` $rhs `:` type($rhs) `)` `:` type($result) attr-dict
    }];
}

//===----------------------------------------------------------------------===//
// Type Conversion
//===----------------------------------------------------------------------===//

def BF3DRMT_CastOp : BF3DRMT_Op<"cast", [Pure]> {
    let summary = "Type conversion";
    let arguments = (ins AnyType:$value);
    let results = (outs AnyType:$result);
    let assemblyFormat = [{
        `(` $value `:` type($value) `)` `:` type($result) attr-dict
    }];
}

//===----------------------------------------------------------------------===//
// Pipe Operations
// These replace vDRMT table/table_action and model the DOCA Flow pipe API.
// Block successor routing is expressed via bf3drmt.next ops inside the body,
// NOT via explicit fwd_type attributes. This mirrors how vDSA chains blocks.
//===----------------------------------------------------------------------===//

def BF3DRMT_PipeOp : BF3DRMT_Op<"pipe", [Symbol, NoTerminator]> {
    let summary = "DOCA Flow pipe (match-action rule set for the NIC ASIC)";
    let description = [{
        Represents a DOCA Flow pipe — the primary structural unit of the BF3 NIC ASIC
        match-action pipeline.

        pipe_type controls the matching algorithm:
          basic   — exact match; match template uses 0xffffffff for matched fields.
          acl     — ternary match with a SHARED match_mask across ALL entries.
                    (BF3 constraint: unlike standard ternary where each entry has
                    its own mask, all ACL entries must share the pipe-level mask.)
          control — priority-based control pipe; entries can carry comparison
                    conditions (used to approximate vDRMT if/cmp semantics).
          hash    — hash-indexed pipe; entry index = hash(fields) % nr_entries.
          lpm     — longest-prefix-match pipe.

        match_mask (ACL/basic only): a dense array of i64 values, one per matched
        field, encoding the shared ternary mask. For ACL pipes this embeds the
        BF3 constraint that all entries share this mask.

        The body contains bf3drmt.pipe_action ops. Each pipe_action body ends with
        a bf3drmt.next op specifying which vDSA block gets the packet on a hit.

        Example:
        ```mlir
        bf3drmt.pipe @acl_ipv4 type(acl) mask([0xffffffff, 0xffffff00]) nr_entries(1024) {
            bf3drmt.pipe_action @action_allow {
                bf3drmt.assign %val, %dst_ref : i32, !vdrmt.ref<i32>
                bf3drmt.next 2
            }
        }
        bf3drmt.next 0   // miss: go to block 0
        ```
    }];
    let arguments = (ins
        SymbolNameAttr:$sym_name,
        BF3DRMTPipeType:$pipe_type,
        // Shared ternary mask — one i64 per match field (BF3 ACL constraint).
        // Empty for hash/lpm/control pipes.
        OptionalAttr<DenseI64ArrayAttr>:$match_mask,
        OptionalAttr<I32Attr>:$nr_entries,
        OptionalAttr<DictionaryAttr>:$annotations
    );
    let regions = (region SizedRegion<1>:$body);
    let assemblyFormat = [{
        $sym_name `type` `(` $pipe_type `)`
        (`mask` `(` $match_mask^ `)`)?
        (`nr_entries` `(` $nr_entries^ `)`)?
        (`annotations` $annotations^)?
        $body attr-dict
    }];
    let hasVerifier = 1;
}

def BF3DRMT_PipeActionOp : BF3DRMT_Op<"pipe_action",
    [AutomaticAllocationScope]> {
    let summary = "Action executed when a pipe entry matches";
    let description = [{
        Represents the action taken for a matching pipe entry. The body contains
        field modification ops (assign, copy_field, add_to_field) and must end
        with a bf3drmt.next op specifying the successor vDSA block for packets
        that hit this action.

        Forward routing is NOT expressed as attributes here — it is expressed
        via the bf3drmt.next terminator inside the body. The miss successor is
        expressed via a bf3drmt.next after the enclosing pipe op.

        Example:
        ```mlir
        bf3drmt.pipe_action @allow {
            bf3drmt.assign %val, %dst_ref : i32, !vdrmt.ref<i32>
            bf3drmt.next 2     // matched packets go to block 2
        }
        ```
    }];
    let arguments = (ins
        SymbolRefAttr:$action,
        OptionalAttr<DictionaryAttr>:$annotations
    );
    let regions = (region SizedRegion<1>:$body);
    let assemblyFormat = [{
        $action (`annotations` $annotations^)? $body attr-dict
    }];
    let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Field Action Operations
// These correspond to DOCA Flow action descriptors (doca_flow_action_desc).
// They are used inside pipe_action bodies.
//===----------------------------------------------------------------------===//

def BF3DRMT_CopyFieldOp : BF3DRMT_Op<"copy_field"> {
    let summary = "Copy bits from one header/metadata field to another (DOCA FLOW_ACTION_COPY)";
    let description = [{
        Copies `width` bits from `src_ref` starting at `src_bit_offset` into
        `dst_ref` at `dst_bit_offset`. Corresponds to DOCA_FLOW_ACTION_COPY.

        Both src_ref and dst_ref must be references obtained from struct_extract_ref.

        Example:
        ```mlir
        // copy outer.ipv4.version_ihl[0:4] -> meta.u32[0][2:6]
        // (ihl * 4 = IPv4 header length)
        bf3drmt.copy_field %ihl_ref, %meta_ref
            {src_offset = 0, dst_offset = 2, width = 4}
            : !vdrmt.ref<i8>, !vdrmt.ref<i32>
        ```
    }];
    let arguments = (ins
        AnyType:$src_ref,
        AnyType:$dst_ref,
        I32Attr:$src_offset,
        I32Attr:$dst_offset,
        I32Attr:$width
    );
    let assemblyFormat = [{
        $src_ref `,` $dst_ref
        `{` `src_offset` `=` $src_offset `,`
             `dst_offset` `=` $dst_offset `,`
             `width` `=` $width `}`
        attr-dict `:` type($src_ref) `,` type($dst_ref)
    }];
}

def BF3DRMT_AddToFieldOp : BF3DRMT_Op<"add_to_field"> {
    let summary = "Accumulate a value into a header/metadata field (DOCA FLOW_ACTION_ADD)";
    let description = [{
        Adds `width` bits from `src_ref` at `src_bit_offset` into `dst_ref`
        at `dst_bit_offset`. Corresponds to DOCA_FLOW_ACTION_ADD.

        Used to accumulate multiple sub-fields into a metadata word, e.g.:
          copy ipv4.ihl[0:4] -> meta.u32[0][2:6]   (ihl*4 = ipv4 hdr len)
          add  tcp.data_off[0:4] -> meta.u32[0][2:6] (data_off*4 = tcp hdr len)
          result in meta.u32[0] = ipv4_hdr_len + tcp_hdr_len

        Example:
        ```mlir
        bf3drmt.add_to_field %tcp_doff_ref, %meta_ref
            {src_offset = 0, dst_offset = 2, width = 4}
            : !vdrmt.ref<i8>, !vdrmt.ref<i32>
        ```
    }];
    let arguments = (ins
        AnyType:$src_ref,
        AnyType:$dst_ref,
        I32Attr:$src_offset,
        I32Attr:$dst_offset,
        I32Attr:$width
    );
    let assemblyFormat = [{
        $src_ref `,` $dst_ref
        `{` `src_offset` `=` $src_offset `,`
             `dst_offset` `=` $dst_offset `,`
             `width` `=` $width `}`
        attr-dict `:` type($src_ref) `,` type($dst_ref)
    }];
}

//===----------------------------------------------------------------------===//
// Table Key Operations
// Used inside pipe bodies to describe the match-key computation.
//===----------------------------------------------------------------------===//

def BF3DRMT_TableKeyOp : BF3DRMT_Op<"table_key", [NoTerminator]> {
    let summary = "Match-key computation region for a pipe";
    let description = [{
        Contains bf3drmt read operations that extract the field value used as the
        pipe's match key. The region must end with a bf3drmt.match_key op.

        The ops inside the region typically form a struct_extract_ref -> read chain
        that navigates to the specific header field the pipe matches on.

        Example:
        ```mlir
        bf3drmt.table_key {
            %ip_ref   = bf3drmt.struct_extract_ref %hdr_ref[0] : ...
            %proto_ref = bf3drmt.struct_extract_ref %ip_ref[0] : ...
            %proto    = bf3drmt.read %proto_ref : !vdrmt.ref<i8> -> i8
            bf3drmt.match_key %proto : i8
        }
        ```
    }];
    let regions = (region SizedRegion<1>:$body);
    let assemblyFormat = "$body attr-dict";
    let hasVerifier = 1;
}

def BF3DRMT_MatchKeyOp : BF3DRMT_Op<"match_key", [Terminator]> {
    let summary = "Identifies the match-key value; terminates a table_key region";
    let description = [{
        Must appear as the last (terminator) op in a bf3drmt.table_key region.
        Carries the value that will be used as the lookup key for the enclosing
        pipe's match-action table.
    }];
    let arguments = (ins AnyType:$key);
    let assemblyFormat = "$key `:` type($key) attr-dict";
}

//===----------------------------------------------------------------------===//
// Control Flow / Terminators
//===----------------------------------------------------------------------===//

def BF3DRMT_NextOp : BF3DRMT_Op<"next", [Pure, Terminator, ReturnLike]> {
    let summary = "Chain to the next vDSA block (unconditional)";
    let description = [{
        Unconditional successor: `bf3drmt.next N` — forward the packet to vDSA
        block N after completing the current action or function.

        This op is the sole terminator in bf3drmt. It appears:
          - At function scope to express the default (miss) successor.
          - At the end of pipe_action bodies to express the hit successor.

        The conditional form is absent in bf3drmt because the NIC ASIC has no
        general-purpose cmp/if ops. Conditional routing is expressed structurally
        via control pipes and multiple pipe_action entries.
    }];
    let arguments = (ins I32Attr:$successor);
    let assemblyFormat = "$successor attr-dict";
    let builders = [
        OpBuilder<(ins "int32_t":$successor), [{
            build($_builder, $_state, $_builder.getI32IntegerAttr(successor));
        }]>
    ];
}

#endif // BF3DRMTOPS
