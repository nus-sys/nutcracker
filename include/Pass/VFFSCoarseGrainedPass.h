#pragma once
#include "mlir/Pass/Pass.h"

namespace mlir {
void registerVFFSCoarseGrainedPass();
std::unique_ptr<Pass> createVFFSCoarseGrainedPass();
} // namespace mlir
