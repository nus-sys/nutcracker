//===- vDPPDialect.cpp - Virtual DPP dialect ---------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Virtual DPP dialect.
//===----------------------------------------------------------------------===//


void mlir::vdpp::vDPPDialect::initialize() {
    std::cout << "Initializing vDPP Dialect..." << std::endl;
    addOperations<
#define GET_OP_LIST
#include "Dialect/vDPP/IR/vDPPOps.cpp.inc"
        >();
    registerTypes();
    // registerAttributes();
}
