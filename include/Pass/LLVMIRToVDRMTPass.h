#pragma once
// ============================================================================
// LLVMIRToVDRMTPass.h
// LLVM IR frontend: import .ll files and raise to vDRMT dialect via
// structural pattern matching.
//
// Recognised patterns (applied per vDSA block function):
//   1. GEP chain + icmp + cond_br  →  vdrmt.cmp + vdrmt.if{vdrmt.next}
//   2. GEP chain + load + arith + store to metadata  →  vdrmt.assign
//   3. Constant store to metadata  →  vdrmt.constant + vdrmt.assign
//
// LLVM struct types are mapped to vDRMT struct/header types with synthetic
// field names ("field_0", "field_1", …, "__valid" for the last i1 field).
// ============================================================================

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

std::unique_ptr<Pass> createLLVMIRToVDRMTPass();
void registerLLVMIRToVDRMTPass();

} // namespace mlir
