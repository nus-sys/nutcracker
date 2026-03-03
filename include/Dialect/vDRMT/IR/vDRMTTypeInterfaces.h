#ifndef VDRMT_DIALECT_VDRMT_VDRMT_TYPE_INTERFACES_H
#define VDRMT_DIALECT_VDRMT_VDRMT_TYPE_INTERFACES_H

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Types.h"

namespace mlir::vdrmt {

/// Struct defining a field. Used in structs and header
struct FieldInfo {
    mlir::StringAttr name;
    mlir::Type type;
    mlir::DictionaryAttr annotations;

    FieldInfo(mlir::StringAttr name,
                mlir::Type type,
                mlir::DictionaryAttr annotations = {})
        : name(name), type(type),
        annotations(annotations && !annotations.empty() ? annotations : mlir::DictionaryAttr())
    { }
    
    // Equality comparison required for ArrayRef comparison in MLIR type system
    bool operator==(const FieldInfo &other) const {
        return name == other.name && type == other.type && annotations == other.annotations;
    }
    
    bool operator!=(const FieldInfo &other) const {
        return !(*this == other);
    }
};

// Hash function required for LLVM's type hashing
inline ::llvm::hash_code hash_value(const FieldInfo &field) {
    return ::llvm::hash_combine(field.name, field.type, field.annotations);
}

namespace FieldIdImpl {
unsigned getMaxFieldID(::mlir::Type);

std::pair<::mlir::Type, unsigned> getSubTypeByFieldID(::mlir::Type, unsigned fieldID);

::mlir::Type getFinalTypeByFieldID(::mlir::Type type, unsigned fieldID);

std::pair<unsigned, bool> projectToChildFieldID(::mlir::Type, unsigned fieldID, unsigned index);

std::pair<unsigned, unsigned> getIndexAndSubfieldID(::mlir::Type type, unsigned fieldID);

unsigned getFieldID(::mlir::Type type, unsigned index);

unsigned getIndexForFieldID(::mlir::Type type, unsigned fieldID);

}  // namespace FieldIdImpl

}  // mlir::vdrmt

#include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.h.inc"

#endif  // VDRMT_DIALECT_VDRMT_VDRMT_TYPE_INTERFACES_H
