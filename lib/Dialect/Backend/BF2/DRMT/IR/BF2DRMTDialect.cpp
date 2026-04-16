//===- BF2DRMTDialect.cpp - BF3 NIC ASIC DRMT dialect --------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTDialect.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOps.h"
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTTypes.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// BF3 NIC ASIC DRMT dialect.
//===----------------------------------------------------------------------===//

void mlir::bf2drmt::BF2DRMTDialect::initialize() {
    std::cout << "Initializing BF2DRMT Dialect..." << std::endl;
    addOperations<
#define GET_OP_LIST
#include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTOps.cpp.inc"
        >();
    registerTypes();
}
