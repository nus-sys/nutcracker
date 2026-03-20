#pragma once
// ============================================================================
// VDPPToLLVMPass.h
// Lower a vDPP module to LLVM IR for ARM code generation.
//
// The output is one LLVM IR function per vDSA block:
//   define i32 @nc_blockN_arm(ptr %headers, ptr %metadata, ptr %std_meta)
// Returns the successor block ID, or -1 at end of pipeline.
// ============================================================================

#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {

/// Parse the vDPP module at `vdppFilePath`, lower it to LLVM IR, and write
/// the .ll text to `os`.  The entry function is renamed to nc_block<blockId>_arm.
/// Uses an independent MLIRContext, safe to call from within a pass.
LogicalResult emitVDPPAsLLVMIR(int blockId, llvm::StringRef vdppFilePath,
                                llvm::raw_ostream &os);

} // namespace mlir
