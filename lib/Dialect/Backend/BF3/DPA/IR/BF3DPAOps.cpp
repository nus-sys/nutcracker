//===- BF3DPAOps.cpp - BF3 DPA cores dialect ops -------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.h"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include "Dialect/Backend/BF3/DPA/IR/BF3DPADialect.cpp.inc"
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOpsEnums.cpp.inc"

#define GET_OP_CLASSES
#include "Dialect/Backend/BF3/DPA/IR/BF3DPAOps.cpp.inc"

using namespace mlir;
using namespace mlir::bf3dpa;

//===----------------------------------------------------------------------===//
// AllocaOp - PromotableAllocationOpInterface
//===----------------------------------------------------------------------===//

llvm::SmallVector<mlir::MemorySlot> AllocaOp::getPromotableSlots() {
    auto ptrType = mlir::cast<vdpp::PointerType>(getResult().getType());
    return {MemorySlot{getResult(), ptrType.getElementType()}};
}

mlir::Value AllocaOp::getDefaultValue(
    const MemorySlot &slot, OpBuilder &builder) {
    return {};
}

void AllocaOp::handleBlockArgument(
    const MemorySlot &slot, BlockArgument argument, OpBuilder &builder) {}

std::optional<mlir::PromotableAllocationOpInterface>
AllocaOp::handlePromotionComplete(
    const MemorySlot &slot, Value defaultValue, OpBuilder &builder) {
    if (defaultValue && defaultValue.use_empty())
        defaultValue.getDefiningOp()->erase();
    this->erase();
    return {};
}

//===----------------------------------------------------------------------===//
// CondBranchOp - BranchOpInterface
//===----------------------------------------------------------------------===//

SuccessorOperands CondBranchOp::getSuccessorOperands(unsigned index) {
    assert(index < 2 && "invalid successor index");
    return SuccessorOperands(index == 0 ? getTrueDestOperandsMutable()
                                        : getFalseDestOperandsMutable());
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

mlir::LogicalResult ConstantOp::verify() {
    auto typedAttr = mlir::dyn_cast<mlir::TypedAttr>(getValue());
    if (!typedAttr)
        return emitOpError("value must be a typed attribute");
    if (typedAttr.getType() != getRes().getType())
        return emitOpError("value type '") << typedAttr.getType()
               << "' does not match result type '" << getRes().getType() << "'";
    return mlir::success();
}

mlir::OpFoldResult ConstantOp::fold(FoldAdaptor) {
    return getValue();
}

void ConstantOp::getAsmResultNames(
        llvm::function_ref<void(mlir::Value, mlir::StringRef)> setNameFn) {
    setNameFn(getRes(), "cst");
}

//===----------------------------------------------------------------------===//
// ProvisionedPtrOp
//
// Verifier: input and result must have compatible pointer types.
//===----------------------------------------------------------------------===//

mlir::LogicalResult ProvisionedPtrOp::verify() {
    if (getInput().getType() != getResult().getType()) {
        return emitOpError("input type '") << getInput().getType()
               << "' must match result type '" << getResult().getType() << "'";
    }
    // The type must be some form of pointer (vDPP PointerType or integer).
    // We accept any type here and let downstream analysis handle it.
    return mlir::success();
}

//===----------------------------------------------------------------------===//
// LoadOp
//
// Memory partitioning verifier:
//   Trace the ptr back through getelementptr chains. If it originates from a
//   provisioned_ptr, all GEP indices must themselves originate from
//   packet_index ops (direct or through arithmetic on packet_index values).
//
// NOTE: This is a conservative static check. Full provenance analysis is left
// to a dedicated analysis pass. The verifier here enforces the structural
// property that any GEP immediately feeding a load into provisioned memory
// uses a packet_index-annotated index.
//===----------------------------------------------------------------------===//

/// Walk backwards through the def-use chain to check if a value is
/// derived from a bf3dpa.packet_index op.
static bool isPacketDerived(mlir::Value val, int depth = 0) {
    if (depth > 16) return false;  // cycle guard
    mlir::Operation *defOp = val.getDefiningOp();
    if (!defOp) return false;  // block argument — could be packet field; conservative: false
    if (mlir::isa<PacketIndexOp>(defOp)) return true;
    if (mlir::isa<ThreadIdOp>(defOp)) return true;
    // Allow arithmetic on packet-derived values.
    if (mlir::isa<AddOp, SubOp, MulOp,
                  AndOp, LShrOp, AShrOp,
                  ShlOp, CastOp>(defOp)) {
        for (mlir::Value operand : defOp->getOperands()) {
            if (isPacketDerived(operand, depth + 1)) return true;
        }
    }
    // Constants are fine as additive offsets.
    if (mlir::isa<ConstantOp>(defOp)) return true;
    return false;
}

/// Check if a pointer traces back through GEP to a provisioned_ptr,
/// and if so whether all GEP indices are packet-derived.
static mlir::LogicalResult checkPtrPartition(mlir::Value ptr,
                                               mlir::Operation *loc,
                                               int depth = 0) {
    if (depth > 32) return mlir::success();  // give up; conservative pass
    mlir::Operation *defOp = ptr.getDefiningOp();
    if (!defOp) return mlir::success();  // block argument — assume valid

    if (mlir::isa<AllocaOp>(defOp)) {
        return mlir::success();  // local stack — no constraint
    }
    if (mlir::isa<ProvisionedPtrOp>(defOp)) {
        return mlir::success();  // base provisioned ptr — constraint satisfied at GEP
    }
    if (auto gepOp = mlir::dyn_cast<GetElementPtrOp>(defOp)) {
        // Check if base ultimately comes from provisioned memory.
        // If so, all indices must be packet-derived.
        mlir::Value base = gepOp.getBase();
        mlir::Operation *baseDefOp = base.getDefiningOp();
        bool baseIsProvisioned = false;
        if (baseDefOp && mlir::isa<ProvisionedPtrOp>(baseDefOp)) {
            baseIsProvisioned = true;
        }
        // Recursively check whether base is from provisioned memory.
        if (!baseIsProvisioned) {
            // If the deeper base is provisioned, propagate the check.
            // For now: if base is a GEP or provisioned_ptr chain, enforce constraint.
            mlir::Operation *deepBase = base.getDefiningOp();
            while (deepBase && mlir::isa<GetElementPtrOp>(deepBase)) {
                deepBase = mlir::cast<GetElementPtrOp>(deepBase)
                               .getBase().getDefiningOp();
            }
            if (deepBase && mlir::isa<ProvisionedPtrOp>(deepBase))
                baseIsProvisioned = true;
        }
        if (baseIsProvisioned) {
            for (mlir::Value idx : gepOp.getIndices()) {
                if (!isPacketDerived(idx)) {
                    return loc->emitOpError(
                        "memory access into provisioned memory uses a non-packet-derived "
                        "index; all GEP indices into bf3dpa.provisioned_ptr memory must "
                        "originate from bf3dpa.packet_index ops to satisfy the DPA "
                        "memory partitioning constraint");
                }
            }
        }
        return mlir::success();
    }
    return mlir::success();
}

mlir::LogicalResult LoadOp::verify() {
    return checkPtrPartition(getPtr(), *this);
}

mlir::LogicalResult StoreOp::verify() {
    return checkPtrPartition(getPtr(), *this);
}
