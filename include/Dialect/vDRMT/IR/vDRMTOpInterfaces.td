//===- vDRMTOpInterfaces.td - vDRMT VFFA op interfaces ---*- tablegen -*-===//
//
// Defines two op interfaces that capture the common pattern of all
// Fixed-Function Accelerator (VFFA) operations:
//
//   VFFAInstanceOpInterface — ops that declare a named VFFA resource at
//     module scope (counter_instance, hash5tuple_instance, meter_instance,
//     register_instance, and their BF3 counterparts).
//
//   VFFAExecuteOpInterface  — ops that invoke a VFFA instance at runtime
//     (counter.count, hash5tuple.apply, meter.execute, register.read/write).
//     Captures the pattern: some inputs → fixed algorithm → some outputs.
//
// The coarse-grained finalization step inside P4HIRToVDRMT uses these
// interfaces to operate on all VFFA types generically rather than
// enumerating each type.
//
//===----------------------------------------------------------------------===//

#ifndef VDRMT_OP_INTERFACES_TD
#define VDRMT_OP_INTERFACES_TD

include "mlir/IR/OpBase.td"

// ─── VFFAInstanceOpInterface ─────────────────────────────────────────────────
//
// Implemented by all ops that declare a named VFFA hardware resource.
// Structural passes use this to collect the set of already-declared instances
// without caring about the specific VFFA type.

def VFFAInstanceOpInterface : OpInterface<"VFFAInstanceOpInterface"> {
    let description = [{
        Common interface for ops that declare a named Fixed-Function Accelerator
        (VFFA) resource at module scope. All instance ops carry a symbol name
        and are lifted to the module top-level before target lowering.
    }];
    let cppNamespace = "::mlir::vdrmt";

    let methods = [
        InterfaceMethod<
            "Return the symbol name of this VFFA instance declaration.",
            "mlir::StringAttr", "getSymNameAttr"
        >,
    ];
}

// ─── VFFAExecuteOpInterface ──────────────────────────────────────────────────
//
// Implemented by all ops that invoke a VFFA instance at runtime.
// Models the pattern the user described: some inputs → fixed algorithm → outputs.
//
//   getInstanceAttr() — which hardware resource is being invoked
//   getInputs()       — the data values fed into the fixed function
//   removeHelperAttrs() — strips lifting-phase helper attributes after
//                         instance declarations have been hoisted

def VFFAExecuteOpInterface : OpInterface<"VFFAExecuteOpInterface"> {
    let description = [{
        Common interface for ops that invoke a Fixed-Function Accelerator (VFFA)
        instance at runtime.  Every VFFA follows the same abstract pattern:
        a set of input values feeds a fixed algorithm that may produce output
        values and/or update persistent state in the named instance.

        The interface intentionally abstracts over the specific algorithm so
        structural passes (CoarseGrained lifting, verification, mapping) can
        treat all VFFAs uniformly.
    }];
    let cppNamespace = "::mlir::vdrmt";

    let methods = [
        InterfaceMethod<
            "Return the symbol reference to the VFFA instance being invoked.",
            "mlir::FlatSymbolRefAttr", "getInstanceAttr"
        >,
        InterfaceMethod<
            "Return the data inputs supplied to the fixed-function algorithm. "
            "For ops with a single index operand this is {index}; for ops with "
            "multiple operands (e.g. hash5tuple) all data operands are included.",
            "mlir::ValueRange", "getInputs"
        >,
        InterfaceMethod<
            "Remove any helper attributes that were attached during P4HIR→vDRMT "
            "lowering to carry instance size/type hints. Called by the "
            "CoarseGrained pass after all instance ops have been hoisted.",
            "void", "removeHelperAttrs"
        >,
    ];
}

#endif // VDRMT_OP_INTERFACES_TD
