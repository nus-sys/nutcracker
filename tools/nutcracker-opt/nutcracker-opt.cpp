#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h" 

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);

  registry.insert<P4::P4MLIR::P4HIR::P4HIRDialect>();

  return mlir::asMainReturnCode(
        mlir::MlirOptMain(argc, argv, "NutCracker Compiler", registry)
    );
}
