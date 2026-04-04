add_mlir_library(MLIRVFFSCoarseGrainedPass
  VFFSCoarseGrainedPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRvDRMTDialect
)
