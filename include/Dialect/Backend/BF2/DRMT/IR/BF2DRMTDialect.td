//===- BF2DRMTDialect.td - BF3 NIC ASIC DRMT dialect ------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF2DRMT_DIALECT_BF2DRMT_BF2DRMT_DIALECT_TD
#define BF2DRMT_DIALECT_BF2DRMT_BF2DRMT_DIALECT_TD

include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// BF2DRMT dialect definition.
//===----------------------------------------------------------------------===//

def BF2DRMT_Dialect : Dialect {
    let name = "bf2drmt";
    let summary = "BlueField-3 NIC ASIC match-action dialect.";
    let description = [{
        bf2drmt models the NIC ASIC match-action pipeline on the NVIDIA BlueField-3
        SmartNIC. It is a concrete instance of vDRMT with the following hardware
        constraints:

        1. No bitwise operations (And, Or, Xor) — the NIC ASIC does not support
           them directly. Use slice/concat to decompose them.
        2. No cmp or if operations — conditional decisions are expressed through
           DOCA Flow pipe match entries and forwarding targets.
        3. Struct access must follow the struct_extract_ref -> read order.
           Reading a struct and then extracting a field by value is not supported.
        4. Ternary matching uses a shared mask: all entries in an ACL pipe must
           share the same match_mask (BF3-specific constraint). This differs from
           standard ternary where each entry has its own mask.

        The dialect maps to the DOCA Flow API. A bf2drmt.pipe corresponds to a
        DOCA Flow pipe, and bf2drmt.pipe_action corresponds to a DOCA Flow pipe
        entry's action + forward target.
    }];
    let cppNamespace = "::mlir::bf2drmt";

    let extraClassDeclaration = [{
        mlir::Type parseType(mlir::DialectAsmParser &parser) const override;
        void printType(mlir::Type type, mlir::DialectAsmPrinter &printer) const override;
        void registerTypes();
    }];
}

#endif // BF2DRMT_DIALECT_BF2DRMT_BF2DRMT_DIALECT_TD
