add_mlir_dialect_library(MLIRPDGDialect
  PDGDialect.cpp
  PDGOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/Dialect/PDG

  DEPENDS
  PDG_IncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRInferTypeOpInterface
)
