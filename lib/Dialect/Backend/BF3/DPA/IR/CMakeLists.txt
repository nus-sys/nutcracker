add_mlir_dialect_library(MLIRbf3dpaDialect
  BF3DPADialect.cpp
  BF3DPAOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Dialect/Backend/BF3/DPA

  DEPENDS
  BF3DPA_IncGen
  VDPP_IncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRvDPPDialect
  MLIRInferTypeOpInterface
)
