// VDRMTCoarseGrainedPass.cpp
// Lifts vDRMT VFFA instance declarations to module scope and cleans up
// helper attrs.
//
// After P4HIRToVDRMT, VFFA execute ops (counter.count, hash5tuple.apply,
// meter.execute, register.read/write) carry helper attributes encoding the
// instance size/type that were known at lowering time.  This pass:
//
//  1. Collects all already-declared VFFA instances (VFFAInstanceOpInterface).
//  2. Walks all VFFA execute ops (VFFAExecuteOpInterface) and lifts a module-
//     level instance declaration for each unseen symbol.
//  3. Removes the helper attributes via VFFAExecuteOpInterface::removeHelperAttrs().
//
// Instance creation (step 2) remains type-specific because each VFFA kind has
// different constructor parameters (size, element_width, meter_type, etc.).

#include "Pass/VDRMTCoarseGrainedPass.h"
#include "Pass/VFFSCoarseGrainedPass.h"
#include "Dialect/vDRMT/IR/vDRMTOps.h"
#include "Dialect/vDRMT/IR/vDRMTDialect.h"
#include "Dialect/vDRMT/IR/vDRMTOpInterfaces.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
namespace {

struct VDRMTCoarseGrainedPass
    : PassWrapper<VDRMTCoarseGrainedPass, OperationPass<ModuleOp>> {

    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(VDRMTCoarseGrainedPass)

    StringRef getArgument()  const override { return "vdrmt-coarse-grained"; }
    StringRef getDescription() const override {
        return "Lift vDRMT VFFA instance declarations to module scope";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<vdrmt::vDRMTDialect>();
    }

    void runOnOperation() override {
        ModuleOp mod = getOperation();\
        OpBuilder modBuilder(mod.getBodyRegion());
        modBuilder.setInsertionPointToStart(mod.getBody());

        // ── Step 1: Collect all already-declared VFFA instances ──────────────
        // Uses VFFAInstanceOpInterface so this loop covers every VFFA kind
        // without enumerating them individually.
        DenseSet<StringAttr> declared;
        mod.walk([&](vdrmt::VFFAInstanceOpInterface inst) {
            declared.insert(inst.getSymNameAttr());
        });

        // ── Step 2: Lift missing instance declarations ────────────────────────
        // Each VFFA type needs its own creation logic because the constructor
        // parameters differ (size, element_width, meter_type, nr_entries, …).
        // We still use VFFAExecuteOpInterface to retrieve the instance symbol
        // uniformly; only the modBuilder.create<> call is type-specific.

        // Counter
        DenseMap<StringAttr, int32_t> counterSizes;
        mod.walk([&](vdrmt::CounterCountOp op) {
            auto sym  = op.getInstanceAttr().getAttr();
            auto attr = op->getAttrOfType<IntegerAttr>("vdrmt.counter_size");
            int32_t sz = attr ? (int32_t)attr.getInt() : 1024;
            auto it = counterSizes.find(sym);
            if (it == counterSizes.end() || it->second < sz)
                counterSizes[sym] = sz;
        });
        for (auto &[sym, sz] : counterSizes) {
            if (declared.contains(sym)) continue;
            modBuilder.create<vdrmt::CounterInstanceOp>(
                mod.getLoc(), sym, modBuilder.getI32IntegerAttr(sz));
            declared.insert(sym);
        }

        // Hash5Tuple
        DenseMap<StringAttr, int32_t> hashSizes;
        mod.walk([&](vdrmt::Hash5TupleApplyOp op) {
            auto sym  = op.getInstanceAttr().getAttr();
            auto attr = op->getAttrOfType<IntegerAttr>("vdrmt.hash5tuple_nr_entries");
            int32_t sz = attr ? (int32_t)attr.getInt() : 64;
            auto it = hashSizes.find(sym);
            if (it == hashSizes.end() || it->second < sz)
                hashSizes[sym] = sz;
        });
        for (auto &[sym, sz] : hashSizes) {
            if (declared.contains(sym)) continue;
            modBuilder.create<vdrmt::Hash5TupleInstanceOp>(
                mod.getLoc(), sym, modBuilder.getI32IntegerAttr(sz));
            declared.insert(sym);
        }

        // Meter
        struct MeterInfo { int32_t size; int32_t type; };
        DenseMap<StringAttr, MeterInfo> meterInfos;
        mod.walk([&](vdrmt::MeterExecuteOp op) {
            auto sym      = op.getInstanceAttr().getAttr();
            auto sizeAttr = op->getAttrOfType<IntegerAttr>("vdrmt.meter_size");
            auto typeAttr = op->getAttrOfType<IntegerAttr>("vdrmt.meter_type");
            int32_t sz = sizeAttr ? (int32_t)sizeAttr.getInt() : 1024;
            int32_t mt = typeAttr ? (int32_t)typeAttr.getInt() : 0;
            auto it = meterInfos.find(sym);
            if (it == meterInfos.end())
                meterInfos[sym] = {sz, mt};
            else if (it->second.size < sz)
                it->second.size = sz;
        });
        for (auto &[sym, info] : meterInfos) {
            if (declared.contains(sym)) continue;
            modBuilder.create<vdrmt::MeterInstanceOp>(
                mod.getLoc(), sym,
                modBuilder.getI32IntegerAttr(info.size),
                modBuilder.getI32IntegerAttr(info.type));
            declared.insert(sym);
        }

        // Register
        struct RegInfo { int32_t size; int32_t elementWidth; };
        DenseMap<StringAttr, RegInfo> regInfos;
        auto collectRegInfo = [&](Operation *op, StringAttr sym) {
            auto sizeAttr  = op->getAttrOfType<IntegerAttr>("vdrmt.register_size");
            auto widthAttr = op->getAttrOfType<IntegerAttr>("vdrmt.register_element_width");
            int32_t sz = sizeAttr  ? (int32_t)sizeAttr.getInt()  : 1024;
            int32_t ew = widthAttr ? (int32_t)widthAttr.getInt() : 32;
            if (!regInfos.count(sym))
                regInfos[sym] = {sz, ew};
        };
        mod.walk([&](vdrmt::RegisterReadOp  op) { collectRegInfo(op, op.getInstanceAttr().getAttr()); });
        mod.walk([&](vdrmt::RegisterWriteOp op) { collectRegInfo(op, op.getInstanceAttr().getAttr()); });
        for (auto &[sym, info] : regInfos) {
            if (declared.contains(sym)) continue;
            modBuilder.create<vdrmt::RegisterInstanceOp>(
                mod.getLoc(), sym,
                modBuilder.getI32IntegerAttr(info.size),
                modBuilder.getI32IntegerAttr(info.elementWidth));
            declared.insert(sym);
        }

        // ── Step 3: Remove helper attributes (generic via interface) ──────────
        mod.walk([&](vdrmt::VFFAExecuteOpInterface use) {
            use.removeHelperAttrs();
        });
    }
};

} // namespace

void registerVDRMTCoarseGrainedPass() {
    PassRegistration<VDRMTCoarseGrainedPass>();
}

std::unique_ptr<Pass> createVDRMTCoarseGrainedPass() {
    return std::make_unique<VDRMTCoarseGrainedPass>();
}

// Backwards-compat aliases used by VDRMTToBF3DRMTPass.
void registerVFFSCoarseGrainedPass() { registerVDRMTCoarseGrainedPass(); }
std::unique_ptr<Pass> createVFFSCoarseGrainedPass() { return createVDRMTCoarseGrainedPass(); }

} // namespace mlir
