//===- BF3DRMTDialect.cpp - BF3 NIC ASIC DRMT dialect --------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTTypes.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// BF3 NIC ASIC DRMT dialect.
//===----------------------------------------------------------------------===//

void mlir::bf3drmt::BF3DRMTDialect::initialize() {
    std::cout << "Initializing BF3DRMT Dialect..." << std::endl;
    addOperations<
#define GET_OP_LIST
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTOps.cpp.inc"
        >();
    registerTypes();
}
