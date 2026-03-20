#ifndef EGGLOG_CUSTOM_DEFS_H
#define EGGLOG_CUSTOM_DEFS_H

#include "mlir/IR/Attributes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/AsmParser/AsmParser.h"
#include "Dialect/P4HIR/P4HIR_Attrs.h"
#include "Dialect/P4HIR/P4HIR_Types.h"

#include "Egglog.h"

/** 
 * Parse the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */
mlir::Attribute parseFastMathFlagsAttr(const std::vector<std::string>& split, Egglog& egglog);

/**
 * Serialize the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */
std::vector<std::string> stringifyFastMathFlagsAttr(mlir::Attribute attr, Egglog& egglog);

/** Parse the type (function RankedTensor (IntVec Type) Type) */
mlir::Type parseRankedTensorType(const std::vector<std::string>& split, Egglog& egglog);

/** Serialize the type (function RankedTensor (IntVec Type) Type) */
std::vector<std::string> stringifyRankedTensorType(mlir::Type type, Egglog& egglog);

/** 
 * Parse the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */

mlir::Type parseP4HIRExternType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRExternType(mlir::Type type, Egglog& egglog);

mlir::Attribute parseP4HIRIntAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRIntAttr(mlir::Attribute attr, Egglog& egglog);

mlir::Attribute parseP4HIRBoolAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRBoolAttr(mlir::Attribute attr, Egglog& egglog);

mlir::Attribute parseP4HIRValidityBitAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRValidityBitAttr(mlir::Attribute attr, Egglog& egglog);

/** Parse P4HIR bit type (function p4hir.bits (Int) Type) */
mlir::Type parseP4HIRBitsType(const std::vector<std::string>& split, Egglog& egglog);
/** Stringify P4HIR bit type */
std::vector<std::string> stringifyP4HIRBitsType(mlir::Type type, Egglog& egglog);

mlir::Attribute parseP4HIRMatchKindAttr(const std::vector<std::string>& split, Egglog &egglog);
std::vector<std::string> stringifyP4HIRMatchKindAttr(mlir::Attribute attr, Egglog &egglog);

mlir::Type parseP4HIRValidBitType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRValidBitType(mlir::Type type, Egglog& egglog);

mlir::Type parseP4HIRBoolType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRBoolType(mlir::Type type, Egglog& egglog);

mlir::Type parseP4HIRInfIntType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRInfIntType(mlir::Type type, Egglog& egglog);

mlir::Type parseP4HIRErrorType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyP4HIRErrorType(mlir::Type type, Egglog& egglog);

mlir::Type parseP4HIRHeaderType(const std::vector<std::string> &split, Egglog &egglog);

std::vector<std::string> stringifyP4HIRHeaderType(mlir::Type type, Egglog &egglog);

mlir::Type parseP4HIRStructType(const std::vector<std::string> &split, Egglog &egglog);
std::vector<std::string> stringifyP4HIRStructType(mlir::Type type, Egglog &egglog);

/** Parse P4HIR reference type (function p4hir_ref (Type) Type) */
mlir::Type parseP4HIRReferenceType(const std::vector<std::string>& split, Egglog& egglog);

/** Stringify P4HIR reference type */
std::vector<std::string> stringifyP4HIRReferenceType(mlir::Type type, Egglog& egglog);

/* ------- bf3drmt ------- */
mlir::Attribute parseBf3DrmtIntAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtIntAttr(mlir::Attribute attr, Egglog& egglog);

mlir::Attribute parseBf3DrmtBoolAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtBoolAttr(mlir::Attribute attr, Egglog& egglog);

mlir::Attribute parseBf3DrmtValidityBitAttr(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtValidityBitAttr(mlir::Attribute attr, Egglog& egglog);

mlir::Type parseBf3DrmtBitsType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtBitsType(mlir::Type type, Egglog& egglog);

mlir::Type parseBf3DrmtValidBitType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtValidBitType(mlir::Type type, Egglog& egglog);

mlir::Attribute parseBf3DrmtMatchKindAttr(const std::vector<std::string>& split, Egglog &egglog);
std::vector<std::string> stringifyBf3DrmtMatchKindAttr(mlir::Attribute attr, Egglog &egglog);

mlir::Type parseBf3DrmtBoolType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtBoolType(mlir::Type type, Egglog& egglog);

mlir::Type parseBf3DrmtHeaderType(const std::vector<std::string> &split, Egglog &egglog);
std::vector<std::string> stringifyBf3DrmtHeaderType(mlir::Type type, Egglog &egglog);

mlir::Type parseBf3DrmtStructType(const std::vector<std::string> &split, Egglog &egglog);
std::vector<std::string> stringifyBf3DrmtStructType(mlir::Type type, Egglog &egglog);

mlir::Type parseBf3DrmtReferenceType(const std::vector<std::string>& split, Egglog& egglog);
std::vector<std::string> stringifyBf3DrmtReferenceType(mlir::Type type, Egglog& egglog);

std::vector<std::string> stringifyLLVMPointerType(mlir::Type type, Egglog& egglog);
mlir::Type parseLLVMPointerType(const std::vector<std::string>& data, Egglog& egglog);

std::vector<std::string> stringifyLLVMStructType(mlir::Type type, Egglog& egglog);
mlir::Type parseLLVMStructType(const std::vector<std::string>& split, Egglog& egglog);

std::vector<std::string> stringifyLLVMArrayType(mlir::Type type, Egglog& egglog);
mlir::Type parseLLVMArrayType(const std::vector<std::string>& split, Egglog& egglog);

#endif  // EGGLOG_CUSTOM_DEFS_H