set(LLVM_TARGET_DEFINITIONS vDPP.td)
mlir_tablegen(vDPPDialect.h.inc -gen-dialect-decls -dialect=vdpp)
mlir_tablegen(vDPPDialect.cpp.inc -gen-dialect-defs -dialect=vdpp)
mlir_tablegen(vDPPOps.h.inc -gen-op-decls)
mlir_tablegen(vDPPOps.cpp.inc -gen-op-defs)
mlir_tablegen(vDPPTypes.h.inc -gen-typedef-decls -typedefs-dialect=vdpp)
mlir_tablegen(vDPPTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=vdpp)
add_public_tablegen_target(VDPP_IncGen)
add_dependencies(mlir-headers VDPP_IncGen)

# # Generate extra headers for custom enum and attrs.
# mlir_tablegen(P4HIR_Attrs.h.inc -gen-attrdef-decls -attrdefs-dialect=p4hir)
# mlir_tablegen(P4HIR_Attrs.cpp.inc -gen-attrdef-defs -attrdefs-dialect=p4hir)
# add_public_tablegen_target(P4MLIR_P4HIR_AttrIncGen)
# add_dependencies(mlir-headers P4MLIR_P4HIR_AttrIncGen)

mlir_tablegen(vDPPOpsEnums.h.inc -gen-enum-decls)
mlir_tablegen(vDPPOpsEnums.cpp.inc -gen-enum-defs)
add_public_tablegen_target(VDPP_EnumIncGen)
add_dependencies(mlir-headers VDPP_EnumIncGen)

set(LLVM_TARGET_DEFINITIONS vDPPOpInterfaces.td)
mlir_tablegen(vDPPOpInterfaces.h.inc -gen-op-interface-decls)
mlir_tablegen(vDPPOpInterfaces.cpp.inc -gen-op-interface-defs)
add_public_tablegen_target(VDPP_OpInterfacesIncGen)
add_dependencies(mlir-headers VDPP_OpInterfacesIncGen)

# set(LLVM_TARGET_DEFINITIONS P4HIR_TypeInterfaces.td)
# mlir_tablegen(P4HIR_TypeInterfaces.h.inc -gen-type-interface-decls)
# mlir_tablegen(P4HIR_TypeInterfaces.cpp.inc -gen-type-interface-defs)

# add_public_tablegen_target(P4MLIR_P4HIR_TypeInterfacesIncGen)
# add_dependencies(mlir-headers P4MLIR_P4HIR_TypeInterfacesIncGen)

