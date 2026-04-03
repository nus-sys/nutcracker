#pragma once
#include "mlir/Pass/Pass.h"

namespace mlir {
void registerVDRMTCoarseGrainedPass();
std::unique_ptr<Pass> createVDRMTCoarseGrainedPass();
} // namespace mlir
