add_mlir_library(MLIRVDPPToLLVM
  VDPPToLLVMPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRParser
  MLIRFuncDialect
  MLIRArithDialect
  MLIRLLVMDialect
  MLIRLLVMCommonConversion
  MLIRFuncToLLVM
  MLIRControlFlowToLLVM
  MLIRArithToLLVM
  MLIRTransforms
  MLIRTargetLLVMIRExport
  MLIRLLVMToLLVMIRTranslation
  MLIRBuiltinToLLVMIRTranslation
  MLIRvDPPDialect
)
