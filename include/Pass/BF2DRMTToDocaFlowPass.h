//===- BF2DRMTToDocaFlowPass.h - DOCA Flow C codegen from bf2drmt --------===//
//
// Reads all bf2drmt.mlir files in inputDir and generates
// inputDir/doca_flow_pipeline.c — a self-contained DOCA Flow C source that
// implements the full match-action pipeline as a set of DOCA Flow pipes.
//
//===----------------------------------------------------------------------===//

#ifndef BF2DRMT_TO_DOCA_FLOW_PASS_H
#define BF2DRMT_TO_DOCA_FLOW_PASS_H

#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"

#include <map>

namespace mlir {

/// Generate doca_flow_pipeline.c from the bf2drmt.mlir files in inputDir.
/// The generated file is written to outDir/doca_flow_pipeline.c.
///
/// blockHwMap  : block_id → hardware target (0=DRMT, 1=DPA, 2=ARM).
///               When a successor block is ARM/DPA the pipe emits RSS instead
///               of DOCA_FLOW_FWD_PIPE.  Pass empty map for all-DRMT pipelines.
///
/// armQueueMap : ARM-entrypoint block_id → RSS queue index.
///               Used to emit the correct queue number for DRMT→ARM forwards.
mlir::LogicalResult generateDocaFlowCodeBF2(
    MLIRContext *ctx,
    llvm::StringRef inputDir,
    llvm::StringRef outDir,
    const std::map<int, int> &blockHwMap,
    const std::map<int, int> &armQueueMap);

} // namespace mlir

#endif // BF2DRMT_TO_DOCA_FLOW_PASS_H
