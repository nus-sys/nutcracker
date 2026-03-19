#pragma once

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {

/// Emit standard C for ARM cores for one bf3dpa module (one vDSA block).
/// The generated function returns int: the next block ID (-1 = done).
LogicalResult emitARMFunction(int blockId, ModuleOp mod,
                              llvm::raw_ostream &os);

/// Walk all blockN/bf3dpa.mlir files under inputDir and write
/// arm_handler.c in inputDir.
LogicalResult generateARMCode(MLIRContext *ctx, llvm::StringRef inputDir);

} // namespace mlir
