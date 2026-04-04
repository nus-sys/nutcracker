//===- vDRMTOps.h - Virtual DRMT dialect ops -----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMT_DIALECT_VDRMT_VDRMTOPS_H
#define VDRMT_DIALECT_VDRMT_VDRMTOPS_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "Dialect/vDRMT/IR/vDRMTOpsEnums.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "Dialect/vDRMT/IR/vDRMTOpInterfaces.h"

#define GET_OP_CLASSES
#include "Dialect/vDRMT/IR/vDRMTOps.h.inc"

#endif // VDRMT_DIALECT_VDRMT_VDRMTOPS_H
