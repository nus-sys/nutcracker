#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Pass/P4HIRPartitionPass.h"
#include "Pass/P4HIRToVDRMTPass.h"
#include "Pass/P4HIRToVDPPPass.h"

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);

  registry.insert<P4::P4MLIR::P4HIR::P4HIRDialect>();
  registry.insert<mlir::vdpp::vDPPDialect>();
  registry.insert<mlir::vdrmt::vDRMTDialect>();

  mlir::registerP4HIRPartitionPass();
  mlir::registerP4HIRToVDRMTPass();
  mlir::registerP4HIRToVDPPPass();

  return mlir::asMainReturnCode(
        mlir::MlirOptMain(argc, argv, "NutCracker Compiler", registry)
    );
}
