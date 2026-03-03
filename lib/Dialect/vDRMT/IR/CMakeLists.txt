add_mlir_dialect_library(MLIRvDRMTDialect
  vDRMTDialect.cpp
  vDRMTOps.cpp
  vDRMTTypes.cpp
  vDRMTTypeInterfaces.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/Dialect/vDRMT

  DEPENDS
  VDRMT_IncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRInferTypeOpInterface
)
