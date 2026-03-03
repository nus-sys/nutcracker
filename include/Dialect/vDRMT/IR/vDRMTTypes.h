//===- vDRMTTypes.h - Virtual DRMT dialect types -----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMT_DIALECT_VDRMT_VDRMTTYPES_H
#define VDRMT_DIALECT_VDRMT_VDRMTTYPES_H

// We explicitly do not use push / pop for diagnostic in
// order to propagate pragma further on
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "Dialect/vDRMT/IR/vDRMTOpsEnums.h"
#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h"

#define GET_TYPEDEF_CLASSES
#include "Dialect/vDRMT/IR/vDRMTTypes.h.inc"

#endif // VDRMT_DIALECT_VDRMT_VDRMTTYPES_H
