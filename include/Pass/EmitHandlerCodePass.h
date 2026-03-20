#pragma once

#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

/// Read mapping.txt from vdsa_output, then emit nc_types.h, dpa_handler.c,
/// arm_handler.c, and a Makefile for the selected (minimum-latency) mapping.
/// Only blocks assigned to DPA appear in dpa_handler.c; only ARM blocks in
/// arm_handler.c.  Each block function returns int (next block ID, -1 = done).
std::unique_ptr<Pass> createEmitHandlerCodePass();
void registerEmitHandlerCodePass();

} // namespace mlir
