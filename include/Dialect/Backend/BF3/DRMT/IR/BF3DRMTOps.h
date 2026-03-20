//===- BF3DRMTOps.h - BF3 NIC ASIC DRMT dialect ops -----------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DRMT_DIALECT_BF3DRMT_BF3DRMTOPS_H
#define BF3DRMT_DIALECT_BF3DRMT_BF3DRMTOPS_H

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

// vDRMT types are used as operand/result types in bf3drmt ops.
// bf3drmt ops accept !vdrmt.ref<T>, !vdrmt.struct<...>, etc.
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOpsEnums.h"

#define GET_OP_CLASSES
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h.inc"

#endif // BF3DRMT_DIALECT_BF3DRMT_BF3DRMTOPS_H
