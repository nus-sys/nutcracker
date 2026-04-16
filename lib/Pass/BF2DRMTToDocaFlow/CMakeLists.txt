add_mlir_library(MLIRBf2drmtToDocaFlowCodegen
  DocaFlowCodegen.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRParser
  MLIRSupport
  MLIRbf2drmtDialect
  MLIRBf3drmtToDocaFlowCodegen
)
