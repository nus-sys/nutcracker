#ifndef PASS_P4HIRPARTITIONPASS_H
#define PASS_P4HIRPARTITIONPASS_H

#include "mlir/Pass/Pass.h"

namespace mlir {

std::unique_ptr<Pass> createP4HIRPartitionPass();
void registerP4HIRPartitionPass();

} // namespace mlir

#endif // PASS_P4HIRPARTITIONPASS_H