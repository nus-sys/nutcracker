//===- vDRMTDialect.cpp - Virtual DRMT dialect ---------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"
#include "p4mlir/Dialect/P4HIR/P4HIR_Types.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Virtual DRMT dialect.
//===----------------------------------------------------------------------===//

void mlir::vdrmt::vDRMTDialect::initialize() {
    std::cout << "Initializing vDRMT Dialect..." << std::endl;
    registerTypes();
    addOperations<
#define GET_OP_LIST
#include "Dialect/vDRMT/IR/vDRMTOps.cpp.inc"
        >();
    // registerAttributes();
}
