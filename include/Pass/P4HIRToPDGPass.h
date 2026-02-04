// P4HIRToPDGPass.h - Pass to convert P4 HIR to PDG representation -*- C++ -*-

#ifndef PASS_P4HIRTOPDGPASS_H_
#define PASS_P4HIRTOPDGPASS_H_

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"

namespace {

struct SpillFrameInfo {
    // Map values to slots in the spill frame
    DenseMap<Value, unsigned> valueToFrameSlot;

    // Types of each slot in the frame
    SmallVector<Type, 8> frameSlotTypes;
    
    unsigned allocateSlot(Type type) {
        unsigned slot = frameSlotTypes.size();
        frameSlotTypes.push_back(type);
        return slot;
    }
    
    Type getFrameType(MLIRContext *ctx) const {
        return pdg::SpillFrameType::get(ctx, frameSlotTypes);
    }
};

struct P4HIRToPDGPass : public PassWrapper<P4HIRToPDGPass, 
                                            OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(P4HIRToPDGPass)

    StringRef getArgument() const final { return "p4hir-to-pdg"; }
    StringRef getDescription() const final {
        return "Convert pipeline in P4HIR to PDG";
    }

    void runOnOperation() override;
};

} // namespace