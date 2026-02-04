// PDGOps.h - Program Dependence Graph Operations -*- C++ -*-

#ifndef PDG_DIALECT_PDGOPS_H_
#define PDG_DIALECT_PDGOPS_H_

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"

#define GET_OP_CLASSES
#include "Dialect/PDG/IR/PDGOps.h.inc"

#endif // PDG_DIALECT_PDGOPS_H_