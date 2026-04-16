add_mlir_dialect_library(MLIRbf2drmtDialect
  BF2DRMTDialect.cpp
  BF2DRMTOps.cpp
  BF2DRMTTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Dialect/Backend/BF2/DRMT

  DEPENDS
  BF2DRMT_IncGen
  VDRMT_IncGen
  VDRMT_TypeInterfacesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRvDRMTDialect
  MLIRInferTypeOpInterface
)
