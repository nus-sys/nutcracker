add_mlir_library(MLIRVDRMTToBF2DRMTPass
  VDRMTToBF2DRMTPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDRMTDialect
  MLIRbf2drmtDialect
  MLIRSupport
  MLIRFuncDialect
  MLIRParser
  MLIREqualitySaturationPass
  MLIRBf2drmtToDocaFlowCodegen
)
