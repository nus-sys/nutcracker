add_mlir_library(MLIRBf3dpaToDPACodegen
  DPACodegen.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRParser
  MLIRSupport
  MLIRvDPPDialect
  MLIRbf3dpaDialect
  MLIRFuncDialect
)
