set(LLVM_TARGET_DEFINITIONS vDRMT.td)
mlir_tablegen(vDRMTDialect.h.inc -gen-dialect-decls -dialect=vdrmt)
mlir_tablegen(vDRMTDialect.cpp.inc -gen-dialect-defs -dialect=vdrmt)
mlir_tablegen(vDRMTOps.h.inc -gen-op-decls)
mlir_tablegen(vDRMTOps.cpp.inc -gen-op-defs)
mlir_tablegen(vDRMTTypes.h.inc -gen-typedef-decls -typedefs-dialect=vdrmt)
mlir_tablegen(vDRMTTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=vdrmt)
add_public_tablegen_target(VDRMT_IncGen)
add_dependencies(mlir-headers VDRMT_IncGen)

# Generate extra headers for custom enum and attrs.
mlir_tablegen(vDRMTAttrs.h.inc -gen-attrdef-decls -attrdefs-dialect=vdrmt)
mlir_tablegen(vDRMTAttrs.cpp.inc -gen-attrdef-defs -attrdefs-dialect=vdrmt)
add_public_tablegen_target(VDRMT_AttrIncGen)
add_dependencies(mlir-headers VDRMT_AttrIncGen)

mlir_tablegen(vDRMTOpsEnums.h.inc -gen-enum-decls)
mlir_tablegen(vDRMTOpsEnums.cpp.inc -gen-enum-defs)
add_public_tablegen_target(VDRMT_EnumIncGen)
add_dependencies(mlir-headers VDRMT_EnumIncGen)

# set(LLVM_TARGET_DEFINITIONS P4HIR_TypeInterfaces.td)
# mlir_tablegen(P4HIR_TypeInterfaces.h.inc -gen-type-interface-decls)
# mlir_tablegen(P4HIR_TypeInterfaces.cpp.inc -gen-type-interface-defs)

# add_public_tablegen_target(P4MLIR_P4HIR_TypeInterfacesIncGen)
# add_dependencies(mlir-headers P4MLIR_P4HIR_TypeInterfacesIncGen)

