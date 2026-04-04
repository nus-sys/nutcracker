add_mlir_library(MLIRVDPPToBF3DPAPass
  VDPPToBF3DPAPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDPPDialect
  MLIRbf3dpaDialect
  MLIRSupport
  MLIRFuncDialect
  MLIRParser
  MLIREqualitySaturationPass
  MLIRVDPPToLLVM
)
