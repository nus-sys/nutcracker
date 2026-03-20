add_mlir_library(MLIREqualitySaturationPass
  PARTIAL_SOURCES_INTENDED
  Egglog.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDRMTDialect
  MLIRSupport
  MLIRFuncDialect
  MLIRParser
)
