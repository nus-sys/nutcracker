#pragma once
// ============================================================================
// LLVMIRToVDPPPass.h
// LLVM IR frontend: import .ll files and raise to vDPP dialect.
//
// This pass provides an alternative entry point to the NutCracker pipeline
// for programs written in C/C++ (compiled to LLVM IR by clang) instead of P4.
//
// Each llvm.func in the input becomes one vDSA block (vdpp.mlir) under
// vdsa_output/blockN/, ready for the VDPPToBF3DPA fine-grained lowering pass.
//
// Note: vDRMT (match-action tables) cannot be inferred from LLVM IR because
// it requires domain-specific semantics not present in general-purpose IR.
// Only vDPP (packet data-plane processing) is supported from LLVM IR.
// ============================================================================

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

std::unique_ptr<Pass> createLLVMIRToVDPPPass();
void registerLLVMIRToVDPPPass();

} // namespace mlir
