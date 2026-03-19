add_mlir_dialect_library(MLIRbf3drmtDialect
  BF3DRMTDialect.cpp
  BF3DRMTOps.cpp
  BF3DRMTTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Dialect/Backend/BF3/DRMT

  DEPENDS
  BF3DRMT_IncGen
  VDRMT_IncGen
  VDRMT_TypeInterfacesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRvDRMTDialect
  MLIRInferTypeOpInterface
)
