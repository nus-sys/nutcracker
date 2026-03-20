#pragma once

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {

/// Emit FlexIO-style restricted C for one bf3dpa module (one vDSA block).
/// The generated event handler goes into \p os.
///   blockId    — numeric block ID (used for function name)
///   mod        — parsed bf3dpa module
LogicalResult emitDPAEventHandler(int blockId, ModuleOp mod,
                                  llvm::raw_ostream &os);

/// Walk all blockN/bf3dpa.mlir files under inputDir and write
/// dpa_handler.c in inputDir.
LogicalResult generateDPACode(MLIRContext *ctx, llvm::StringRef inputDir);

} // namespace mlir
