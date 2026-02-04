// PDGDialect.cpp

#include <iostream>

#include "Dialect/PDG/PDGDialect.h"
#include "Dialect/PDG/PDGOps.h"

using namespace mlir;
using namespace mlir::pdg;

void PDGDialect::initialize() {
    std::cout << "Initializing PDG Dialect..." << std::endl;
    addOperations<
#define GET_OP_LIST
#include "Dialect/PDG/PDGOps.cpp.inc"
        >();
}
