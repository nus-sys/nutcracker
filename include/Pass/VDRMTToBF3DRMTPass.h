// ============================================================================
// File: include/Pass/VDRMTToBF3DRMTPass.h
// vDRMT to bf3drmt Fine-Grained Lowering Pass - Header
// ============================================================================

#ifndef PASS_VDRMTTOBF3DRMTPASS_H
#define PASS_VDRMTTOBF3DRMTPASS_H

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

std::unique_ptr<Pass> createVDRMTToBF3DRMTPass();

void registerVDRMTToBF3DRMTPass();

} // namespace mlir

#endif // PASS_VDRMTTOBF3DRMTPASS_H
