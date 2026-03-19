//===- BF3DRMTTypes.h - BF3 NIC ASIC DRMT dialect types --------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DRMT_DIALECT_BF3DRMT_BF3DRMTTYPES_H
#define BF3DRMT_DIALECT_BF3DRMT_BF3DRMTTYPES_H

#include "mlir/IR/BuiltinTypes.h"
// FieldInfo is defined in the vDRMT type-interfaces header and reused here.
#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h"

#define GET_TYPEDEF_CLASSES
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTTypes.h.inc"

#endif // BF3DRMT_DIALECT_BF3DRMT_BF3DRMTTYPES_H
