add_mlir_dialect_library(MLIRvDPPDialect
  vDPPDialect.cpp
  vDPPOps.cpp
  vDPPTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/Dialect/vDPP

  DEPENDS
  VDPP_IncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRInferTypeOpInterface
)
