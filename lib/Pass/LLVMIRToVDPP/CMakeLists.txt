add_mlir_library(MLIRLLVMIRToVDPPPass
  LLVMIRToVDPPPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRParser
  MLIRFuncDialect
  MLIRLLVMDialect
  MLIRvDPPDialect
  MLIRTargetLLVMIRImport
  MLIRLLVMIRToLLVMTranslation
)
