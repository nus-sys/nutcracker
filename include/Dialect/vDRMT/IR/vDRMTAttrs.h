//===- vDRMTAttrs.h - Virtual DRMT dialect attributes -----------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMT_DIALECT_VDRMT_VDRMTATTRS_H
#define VDRMT_DIALECT_VDRMT_VDRMTATTRS_H

// We explicitly do not use push / pop for diagnostic in
// order to propagate pragma further on
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "mlir/IR/BuiltinAttributes.h"
#include "Dialect/vDRMT/IR/vDRMTTypes.h"

namespace mlir::edamlir::vdrmt::detail {
struct IntAttrStorage : public ::mlir::AttributeStorage {
    using KeyTy = std::tuple<::mlir::Type, llvm::APInt>;
    IntAttrStorage(::mlir::Type type, llvm::APInt value)
        : type(std::move(type)), value(std::move(value)) {}

    KeyTy getAsKey() const { return KeyTy(type, value); }

    bool operator==(const KeyTy &rhs) const {
        auto rhsType = std::get<0>(rhs);
        const auto &rhsValue = std::get<1>(rhs);
        return (type == rhsType) &&
               (value.getBitWidth() == rhsValue.getBitWidth() && value == rhsValue);
    }

    static ::llvm::hash_code hashKey(const KeyTy &tblgenKey) {
        return ::llvm::hash_combine(std::get<0>(tblgenKey), std::get<1>(tblgenKey));
    }

    static IntAttrStorage *construct(::mlir::AttributeStorageAllocator &allocator,
                                     KeyTy &&tblgenKey) {
        auto type = std::move(std::get<0>(tblgenKey));
        auto value = std::move(std::get<1>(tblgenKey));
        return new (allocator.allocate<IntAttrStorage>())
            IntAttrStorage(std::move(type), std::move(value));
    }

    mlir::Type type;
    llvm::APInt value;
};

}  // namespace mlir::edamlir::vdrmt::detail

#define GET_ATTRDEF_CLASSES
#include "Dialect/vDRMT/IR/vDRMTAttrs.h.inc"

#endif  // VDRMT_DIALECT_VDRMT_VDRMTATTRS_H