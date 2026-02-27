#ifndef VDPP_DIALECT_VDPP_VDPP_DIALECT_TD
#define VDPP_DIALECT_VDPP_VDPP_DIALECT_TD

include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// DPP dialect definition.
//===----------------------------------------------------------------------===//

def vDPP_Dialect : Dialect {
    let name = "vdpp";
    let summary = "Virtual DPP IR.";
    let description = [{
        vDPP (Virtual datapath processor IR) aims to abstract datapath processor operations,
        and closely model the datapath processor architectural behavior.
    }];
    let cppNamespace = "::mlir::vdpp";

    let extraClassDeclaration = [{
        void printType(mlir::Type type, mlir::DialectAsmPrinter &printer) const override;
        void registerTypes();
    }];
}

#endif // VDPP_DIALECT_VDPP_VDPP_DIALECT_TD