#ifndef VDRMT_DIALECT_VDRMT_VDRMT_DIALECT_TD
#define VDRMT_DIALECT_VDRMT_VDRMT_DIALECT_TD

include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// DRMT dialect definition.
//===----------------------------------------------------------------------===//

def vDRMT_Dialect : Dialect {
    let name = "vdrmt";
    let summary = "Virtual dRMT IR.";
    let description = [{
        vDRMT (Virtual dRMT IR) aims to abstract dRMT operations,
        and closely model the dRMT architectural behavior.
    }];
    let cppNamespace = "::mlir::vdrmt";

    let extraClassDeclaration = [{
        mlir::Type parseType(mlir::DialectAsmParser &parser) const override;
        void printType(mlir::Type type, mlir::DialectAsmPrinter &printer) const override;
        void registerTypes();
    }];
}

#endif // VDRMT_DIALECT_VDRMT_VDRMT_DIALECT_TD