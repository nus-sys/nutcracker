// PDGOps.td - PDG Operations and Types  -*- tablegen -*-

#ifndef PDG_DIALECT_PDGDIALECT_TD_
#define PDG_DIALECT_PDGDIALECT_TD_

include "mlir/IR/OpBase.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/LoopLikeInterface.td"
include "mlir/Interfaces/MemorySlotInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

include "Dialect/PDG/IR/PDGDialect.td"

// Base PDG Operations
class PDG_Op<string mnemonic, list<Trait> traits = []> :
        Op<PDG_Dialect, mnemonic, traits>;

def PDG_GraphOp : PDG_Op<"graph", [NoTerminator, SymbolTable]> {
    let summary = "Top-level container for a pipeline";
    let arguments = (ins SymbolNameAttr:$sym_name);
    let regions = (region SizedRegion<1>:$body);
    let assemblyFormat = "$sym_name $body attr-dict";
}

#endif // PDG_DIALECT_PDGDIALECT_TD_