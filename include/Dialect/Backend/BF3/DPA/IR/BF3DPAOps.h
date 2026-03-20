//===- BF3DPAOps.h - BF3 DPA cores dialect ops ----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DPA_DIALECT_BF3DPA_BF3DPAOPS_H
#define BF3DPA_DIALECT_BF3DPA_BF3DPAOPS_H

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

// vDPP pointer types are used as operand/result types in bf3dpa ops.
#include "Dialect/vDPP/IR/vDPPTypes.h"

#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOpsEnums.h"

#define GET_OP_CLASSES
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h.inc"

#endif // BF3DPA_DIALECT_BF3DPA_BF3DPAOPS_H
