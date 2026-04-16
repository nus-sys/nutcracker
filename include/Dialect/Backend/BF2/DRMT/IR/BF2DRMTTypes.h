//===- BF2DRMTTypes.h - BF3 NIC ASIC DRMT dialect types --------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF2DRMT_DIALECT_BF2DRMT_BF2DRMTTYPES_H
#define BF2DRMT_DIALECT_BF2DRMT_BF2DRMTTYPES_H

#include "mlir/IR/BuiltinTypes.h"
// FieldInfo is defined in the vDRMT type-interfaces header and reused here.
#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h"

#define GET_TYPEDEF_CLASSES
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.h.inc"

#endif // BF2DRMT_DIALECT_BF2DRMT_BF2DRMTTYPES_H
