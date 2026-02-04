#ifndef PDG_DIALECT_PDGDIALECT_TD_
#define PDG_DIALECT_PDGDIALECT_TD_

include "mlir/IR/OpBase.td"

// PDG Dialect Definition

def PDGDialect : Dialect {
  let name = "pdg";
  let cppNamespace = "mlir::pdg";
  let summary = "Program Dependence Graph Dialect";
  let description = [{
    The PDG dialect provides operations and types for representing
    program dependence graphs within MLIR.
  }];
}

#endif // PDG_DIALECT_PDG_TD_