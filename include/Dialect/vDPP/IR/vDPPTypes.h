//===- vDPPTypes.h - Virtual DPP dialect types -----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDPP_DIALECT_VDPP_VDPP_TYPES_H
#define VDPP_DIALECT_VDPP_VDPP_TYPES_H

// We explicitly do not use push / pop for diagnostic in
// order to propagate pragma further on
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "Dialect/vDPP/IR/vDPPOpsEnums.h"

#define GET_TYPEDEF_CLASSES
#include "Dialect/vDPP/IR/vDPPTypes.h.inc"

#endif // VDPP_DIALECT_VDPP_VDPP_TYPES_H