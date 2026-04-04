#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "p4mlir/Dialect/P4HIR/P4HIR_Dialect.h"
#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/Backend/BF3/DRMT/IR/BF3DRMTDialect.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.h"
#include "Pass/P4HIRPartitionPass.h"
#include "Pass/P4HIRToVDRMTPass.h"
#include "Pass/P4HIRToVDPPPass.h"
#include "Pass/VDRMTToBF3DRMTPass.h"
#include "Pass/VDPPToBF3DPAPass.h"
#include "Pass/EmitHandlerCodePass.h"
#include "Pass/LLVMIRToVDPPPass.h"
#include "Pass/LLVMIRToVDRMTPass.h"
#include "Pass/VDRMTCoarseGrainedPass.h"

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);

  registry.insert<P4::P4MLIR::P4HIR::P4HIRDialect>();
  registry.insert<mlir::vdpp::vDPPDialect>();
  registry.insert<mlir::vdrmt::vDRMTDialect>();
  registry.insert<mlir::bf3drmt::BF3DRMTDialect>();
  registry.insert<mlir::bf3dpa::BF3DPADialect>();
  mlir::registerP4HIRPartitionPass();
  mlir::registerP4HIRToVDRMTPass();
  mlir::registerP4HIRToVDPPPass();
  mlir::registerVDRMTToBF3DRMTPass();
  mlir::registerVDPPToBF3DPAPass();
  mlir::registerEmitHandlerCodePass();
  mlir::registerLLVMIRToVDPPPass();
  mlir::registerLLVMIRToVDRMTPass();
  mlir::registerVDRMTCoarseGrainedPass();

  return mlir::asMainReturnCode(
        mlir::MlirOptMain(argc, argv, "NutCracker Compiler", registry)
    );
}
