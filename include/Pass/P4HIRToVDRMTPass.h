// ============================================================================
// File: include/Pass/P4HIRToVDRMTPass.h
// P4HIR to vDRMT Lowering Pass - Header
// ============================================================================

#ifndef PASS_P4HIRTOVDRMTPASS_H
#define PASS_P4HIRTOVDRMTPASS_H

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

std::unique_ptr<Pass> createP4HIRToVDRMTPass();

void registerP4HIRToVDRMTPass();

} // namespace mlir

#endif // PASS_P4HIRTOVDRMTPASS_H