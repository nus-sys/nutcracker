add_mlir_library(MLIRVDRMTToBF3DRMTPass
  VDRMTToBF3DRMTPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDRMTDialect
  MLIRbf3drmtDialect
  MLIRSupport
  MLIRFuncDialect
  MLIRParser
  MLIREqualitySaturationPass
  MLIRBf3drmtToDocaFlowCodegen
  MLIRVFFSCoarseGrainedPass
)
