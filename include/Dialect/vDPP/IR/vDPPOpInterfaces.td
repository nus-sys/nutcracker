//===- vDPPOpInterfaces.td - vDPP VFFA op interfaces ---*- tablegen -*-===//
//
// Defines two op interfaces that capture the common pattern of all
// Fixed-Function Accelerator (VFFA) operations in vDPP. These mirror the
// vdrmt::VFFA* interfaces in shape but live in vDPP's own namespace so the
// two virtual dialects stay independent.
//
//   VFFAInstanceOpInterface — ops that declare a named VFFA resource at
//     module scope (counter_instance, hash5tuple_instance, sha_instance,
//     regex_instance, compress_instance).
//
//   VFFAExecuteOpInterface  — ops that invoke a VFFA instance at runtime.
//
//===----------------------------------------------------------------------===//

#ifndef VDPP_OP_INTERFACES_TD
#define VDPP_OP_INTERFACES_TD

include "mlir/IR/OpBase.td"

def VFFAInstanceOpInterface : OpInterface<"VFFAInstanceOpInterface"> {
    let description = [{
        Common interface for ops that declare a named Fixed-Function Accelerator
        (VFFA) resource at module scope. All instance ops carry a symbol name
        and are lifted to the module top-level before target lowering.
    }];
    let cppNamespace = "::mlir::vdpp";

    let methods = [
        InterfaceMethod<
            "Return the symbol name of this VFFA instance declaration.",
            "mlir::StringAttr", "getSymNameAttr"
        >,
    ];
}

def VFFAExecuteOpInterface : OpInterface<"VFFAExecuteOpInterface"> {
    let description = [{
        Common interface for ops that invoke a Fixed-Function Accelerator (VFFA)
        instance at runtime. Every VFFA follows the same abstract pattern:
        a set of input values feeds a fixed algorithm that may produce output
        values and/or update persistent state in the named instance.
    }];
    let cppNamespace = "::mlir::vdpp";

    let methods = [
        InterfaceMethod<
            "Return the symbol reference to the VFFA instance being invoked.",
            "mlir::FlatSymbolRefAttr", "getInstanceAttr"
        >,
        InterfaceMethod<
            "Return the data inputs supplied to the fixed-function algorithm.",
            "mlir::ValueRange", "getInputs"
        >,
        InterfaceMethod<
            "Remove any helper attributes that were attached during lowering.",
            "void", "removeHelperAttrs"
        >,
    ];
}

#endif // VDPP_OP_INTERFACES_TD
