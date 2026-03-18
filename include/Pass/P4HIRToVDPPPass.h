//===- P4HIRToVDPPPass.h - P4HIR to vDPP Lowering Pass ----------*- C++ -*-===//
//
// Lowers partitioned P4HIR blocks to vDPP (Vector Data Plane Processor) IR
//
//===----------------------------------------------------------------------===//

#ifndef NUTCRACKER_PASS_P4HIRTOVDPP_H
#define NUTCRACKER_PASS_P4HIRTOVDPP_H

#include "mlir/Pass/Pass.h"
#include <memory>

namespace mlir {

/// Creates a pass that lowers P4HIR to vDPP dialect
std::unique_ptr<Pass> createP4HIRToVDPPPass();

/// Registers the P4HIR to vDPP lowering pass
void registerP4HIRToVDPPPass();

} // namespace mlir

#endif // NUTCRACKER_PASS_P4HIRTOVDPP_H