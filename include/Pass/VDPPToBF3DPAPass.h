#pragma once

#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

namespace mlir {

/// Top-level entry point: process all block directories under inputDir,
/// lowering each vdpp.mlir → bf3dpa.mlir, appending DPA block entries to
/// mapper.egg, and emitting dpa_handler.c / arm_handler.c.
LogicalResult generateBF3DPACode(MLIRContext *ctx, llvm::StringRef inputDir);

std::unique_ptr<Pass> createVDPPToBF3DPAPass();
void registerVDPPToBF3DPAPass();

} // namespace mlir
