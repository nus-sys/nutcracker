add_mlir_library(MLIRP4HIRPartitionPass
  P4HIRPartitionPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDRMTDialect
  MLIRvDPPDialect
  MLIRSupport
)

# Link external P4MLIR library
target_link_libraries(MLIRP4HIRPartitionPass PRIVATE ${LIB_P4HIR})