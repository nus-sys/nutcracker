//===- BF2DRMTOps.h - BF3 NIC ASIC DRMT dialect ops -----------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF2DRMT_DIALECT_BF2DRMT_BF2DRMTOPS_H
#define BF2DRMT_DIALECT_BF2DRMT_BF2DRMTOPS_H

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

// vDRMT types are used as operand/result types in bf2drmt ops.
// bf2drmt ops accept !vdrmt.ref<T>, !vdrmt.struct<...>, etc.
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOpsEnums.h"

#define GET_OP_CLASSES
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOps.h.inc"

#endif // BF2DRMT_DIALECT_BF2DRMT_BF2DRMTOPS_H
