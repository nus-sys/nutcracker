//===- vDPPOps.h - Virtual DPP dialect ops -----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDPP_DIALECT_VDPP_VDPPOPS_H
#define VDPP_DIALECT_VDPP_VDPPOPS_H

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

#include "Dialect/vDPP/IR/vDPPOpsEnums.h"
#include "Dialect/vDPP/IR/vDPPOpInterfaces.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

#define GET_OP_CLASSES
#include "Dialect/vDPP/IR/vDPPOps.h.inc"

#endif // VDPP_DIALECT_VDPP_VDPPOPS_H
