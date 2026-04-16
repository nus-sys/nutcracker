get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
set(LIBS
        ${dialect_libs}
        ${conversion_libs}
        MLIRTransforms
        MLIROptLib
        ${LIB_P4HIR}
        MLIRvDRMTDialect
        MLIRbf3drmtDialect
        MLIRbf3dpaDialect
        MLIRbf2drmtDialect
        MLIRP4HIRPartitionPass
        MLIRP4HIRToVDRMTPass
        MLIRP4HIRToVDPPPass
        MLIRVDRMTToBF3DRMTPass
        MLIRVDRMTToBF2DRMTPass
        MLIRBf2drmtToDocaFlowCodegen
        MLIRVDPPToBF3DPAPass
        MLIREmitHandlerCodePass
        MLIRLLVMIRToVDPPPass
        MLIRLLVMIRToVDRMTPass
        MLIRTargetLLVMIRImport
        #MLIRP4HIRToDPUPass
        #MLIRP4HIRToDRMTPass
        )
add_llvm_executable(nutcracker-opt nutcracker-opt.cpp)

llvm_update_compile_flags(nutcracker-opt)
target_link_libraries(nutcracker-opt PRIVATE ${LIBS})

mlir_check_all_link_libraries(nutcracker-opt)
