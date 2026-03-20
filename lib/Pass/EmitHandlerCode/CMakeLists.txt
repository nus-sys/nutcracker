add_mlir_library(MLIREmitHandlerCodePass
  EmitHandlerCodePass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRParser
  MLIRFuncDialect
  MLIRvDPPDialect
  MLIRbf3dpaDialect
  MLIRBf3dpaToDPACodegen
  MLIRBf3dpaToARMCodegen
)
