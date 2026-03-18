add_mlir_library(MLIRP4HIRToVDRMTPass
  P4HIRToVDRMTPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Pass

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRTransforms
  MLIRAnalysis
  MLIRvDRMTDialect
  MLIRSupport
)

# Link external P4MLIR library
target_link_libraries(MLIRP4HIRToVDRMTPass PRIVATE ${LIB_P4HIR})