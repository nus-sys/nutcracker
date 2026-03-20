//===- BF3DPADialect.cpp - BF3 DPA cores dialect -------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// BF3 DPA cores dialect.
//===----------------------------------------------------------------------===//

void mlir::bf3dpa::BF3DPADialect::initialize() {
    std::cout << "Initializing BF3DPA Dialect..." << std::endl;
    addOperations<
#define GET_OP_LIST
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.cpp.inc"
        >();
}
