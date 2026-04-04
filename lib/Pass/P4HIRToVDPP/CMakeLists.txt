add_mlir_library(MLIRP4HIRToVDPPPass
  P4HIRToVDPPPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDPPDialect
  MLIRSupport
)

# Link P4MLIR transforms library for FlattenCFGPass
target_link_libraries(MLIRP4HIRToVDPPPass PRIVATE ${LIB_TRANSFORMS})

# Link external P4MLIR library
target_link_libraries(MLIRP4HIRToVDPPPass PRIVATE ${LIB_P4HIR})