// ============================================================================
// File: include/Pass/VDRMTToBF2DRMTPass.h
// vDRMT to bf2drmt Fine-Grained Lowering Pass - Header
// ============================================================================

#ifndef PASS_VDRMTTOBF2DRMTPASS_H
#define PASS_VDRMTTOBF2DRMTPASS_H

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

std::unique_ptr<Pass> createVDRMTToBF2DRMTPass();

void registerVDRMTToBF2DRMTPass();

} // namespace mlir

#endif // PASS_VDRMTTOBF2DRMTPASS_H
