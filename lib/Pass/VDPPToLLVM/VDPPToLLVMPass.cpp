// ============================================================================
// VDPPToLLVM — lower a vDPP module to LLVM IR for ARM code generation.
//
// Pipeline:
//   Step 1 – preprocess: fix func signature (void→i32), replace vdpp.return
//             with func.return carrying the successor constant.
//   Step 2 – dialect conversion: vDPP types/ops → LLVM dialect types/ops.
//   Step 3 – standard FuncToLLVM conversion.
//   Step 4 – translateModuleToLLVMIR → print .ll text.
//
// Output function signature:
//   define i32 @nc_blockN_arm(ptr %headers, ptr %metadata, ptr %std_meta)
// Returns successor block ID, or -1 at end of pipeline.
// ============================================================================

#include "Pass/VDPPToLLVMPass.h"

#include "Dialect/vDPP/IR/vDPPDialect.h"
#include "Dialect/vDPP/IR/vDPPOps.h"
#include "Dialect/vDPP/IR/vDPPTypes.h"

#include "mlir/Parser/Parser.h"

#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"

#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::LLVM;

// ============================================================================
// Step 1 – Preprocess: fix function signature and vdpp.return → func.return
// ============================================================================

/// Convert vdpp.return {successor=N} → llvm.return %N_const : i32.
/// For the bare func.return (no successor), return -1.
/// Uses LLVM ops directly so no additional conversion patterns are needed.
static void rewriteReturns(func::FuncOp funcOp, OpBuilder &builder) {
    auto i32 = builder.getI32Type();

    funcOp.walk([&](vdpp::ReturnOp retOp) {
        builder.setInsertionPoint(retOp);
        int32_t succ = retOp.getSuccessor().value_or(-1);
        Value succVal = builder.create<LLVM::ConstantOp>(
            retOp.getLoc(), i32, builder.getI32IntegerAttr(succ));
        builder.create<LLVM::ReturnOp>(retOp.getLoc(), succVal);
        retOp.erase();
    });

    // Also handle bare func.return (no value) if any remain.
    funcOp.walk([&](func::ReturnOp retOp) {
        if (retOp.getNumOperands() == 0) {
            builder.setInsertionPoint(retOp);
            Value neg1 = builder.create<LLVM::ConstantOp>(
                retOp.getLoc(), i32, builder.getI32IntegerAttr(-1));
            builder.create<LLVM::ReturnOp>(retOp.getLoc(), neg1);
            retOp.erase();
        }
    });
}

/// Change the func.func's return type from () to i32 and rename it.
static void fixFuncSignature(func::FuncOp funcOp, int blockId) {
    MLIRContext *ctx = funcOp.getContext();
    auto oldFnTy = funcOp.getFunctionType();
    auto newFnTy = FunctionType::get(ctx, oldFnTy.getInputs(),
                                     {IntegerType::get(ctx, 32)});
    funcOp.setFunctionType(newFnTy);
    funcOp.setName("nc_block" + std::to_string(blockId) + "_arm");
}

static LogicalResult preprocessModule(ModuleOp mod, int blockId) {
    OpBuilder builder(mod.getContext());
    bool found = false;
    for (auto funcOp : mod.getOps<func::FuncOp>()) {
        rewriteReturns(funcOp, builder);
        fixFuncSignature(funcOp, blockId);
        found = true;
    }
    if (!found) {
        llvm::errs() << "  ✗ VDPPToLLVM: no func.func found in block "
                     << blockId << " vdpp.mlir\n";
        return failure();
    }
    return success();
}

// ============================================================================
// Step 2 – Dialect conversion: vDPP ops → LLVM dialect ops
// ============================================================================

/// Extend LLVMTypeConverter to handle vdpp-specific types.
struct VDPPTypeConverter : public LLVMTypeConverter {
    VDPPTypeConverter(MLIRContext *ctx) : LLVMTypeConverter(ctx) {
        // vdpp.ptr → !llvm.ptr (opaque)
        addConversion([](vdpp::PointerType t) -> Type {
            return LLVMPointerType::get(t.getContext());
        });

        // vdpp.struct<"name", (fields...)> → !llvm.struct<"name", (fields...)>
        addConversion([this](vdpp::StructType structTy) -> Type {
            SmallVector<Type> convertedFields;
            for (Type fieldTy : structTy.getBody()) {
                Type c = convertType(fieldTy);
                if (!c) return nullptr;
                convertedFields.push_back(c);
            }
            if (structTy.isIdentified()) {
                auto llvmStruct = LLVM::LLVMStructType::getIdentified(
                    structTy.getContext(), structTy.getNameStr());
                if (llvmStruct.setBody(convertedFields,
                                       structTy.getIsPacked()).failed())
                    return nullptr;
                return llvmStruct;
            }
            return LLVM::LLVMStructType::getLiteral(
                structTy.getContext(), convertedFields, structTy.getIsPacked());
        });
    }
};

// ── Arithmetic ──────────────────────────────────────────────────────────────

template <typename SrcOp, typename DstOp>
struct BinOpLowering : public OpConversionPattern<SrcOp> {
    using OpConversionPattern<SrcOp>::OpConversionPattern;
    LogicalResult matchAndRewrite(SrcOp op,
                                  typename SrcOp::Adaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        Type resType = this->getTypeConverter()->convertType(op.getResult().getType());
        rw.replaceOpWithNewOp<DstOp>(op, resType, adaptor.getLhs(), adaptor.getRhs());
        return success();
    }
};

// Shift ops have different operand names (value/amount vs lhs/rhs).
template <typename SrcOp, typename DstOp>
struct ShiftOpLowering : public OpConversionPattern<SrcOp> {
    using OpConversionPattern<SrcOp>::OpConversionPattern;
    LogicalResult matchAndRewrite(SrcOp op,
                                  typename SrcOp::Adaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        Type resType = this->getTypeConverter()->convertType(op.getResult().getType());
        rw.replaceOpWithNewOp<DstOp>(op, resType, adaptor.getValue(), adaptor.getAmount());
        return success();
    }
};

// ── Constant ────────────────────────────────────────────────────────────────

struct ConstantOpLowering : public OpConversionPattern<vdpp::ConstantOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::ConstantOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        Type resType = getTypeConverter()->convertType(op.getRes().getType());
        if (!resType)
            return failure();
        rw.replaceOpWithNewOp<LLVM::ConstantOp>(op, resType, op.getValue());
        return success();
    }
};

// ── ICmp ────────────────────────────────────────────────────────────────────

static LLVM::ICmpPredicate vdppPredToLLVM(vdpp::ICmpPredicate pred) {
    switch (pred) {
    case vdpp::ICmpPredicate::eq:  return LLVM::ICmpPredicate::eq;
    case vdpp::ICmpPredicate::ne:  return LLVM::ICmpPredicate::ne;
    case vdpp::ICmpPredicate::slt: return LLVM::ICmpPredicate::slt;
    case vdpp::ICmpPredicate::sle: return LLVM::ICmpPredicate::sle;
    case vdpp::ICmpPredicate::sgt: return LLVM::ICmpPredicate::sgt;
    case vdpp::ICmpPredicate::sge: return LLVM::ICmpPredicate::sge;
    case vdpp::ICmpPredicate::ult: return LLVM::ICmpPredicate::ult;
    case vdpp::ICmpPredicate::ule: return LLVM::ICmpPredicate::ule;
    case vdpp::ICmpPredicate::ugt: return LLVM::ICmpPredicate::ugt;
    case vdpp::ICmpPredicate::uge: return LLVM::ICmpPredicate::uge;
    }
    return LLVM::ICmpPredicate::eq;
}

struct ICmpOpLowering : public OpConversionPattern<vdpp::ICmpOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::ICmpOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        rw.replaceOpWithNewOp<LLVM::ICmpOp>(
            op, vdppPredToLLVM(op.getPredicate()),
            adaptor.getLhs(), adaptor.getRhs());
        return success();
    }
};

// ── Memory ──────────────────────────────────────────────────────────────────

struct AllocaOpLowering : public OpConversionPattern<vdpp::AllocaOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::AllocaOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        auto ptrType = LLVMPointerType::get(op.getContext());
        auto ptrTy = cast<vdpp::PointerType>(op.getResult().getType());
        Type elemLLVMTy = getTypeConverter()->convertType(ptrTy.getElementType());
        if (!elemLLVMTy) return failure();
        Value one = rw.create<LLVM::ConstantOp>(op.getLoc(),
            rw.getI64Type(), rw.getI64IntegerAttr(1));
        rw.replaceOpWithNewOp<LLVM::AllocaOp>(op, ptrType, elemLLVMTy, one,
                                              op.getAlignment());
        return success();
    }
};

struct LoadOpLowering : public OpConversionPattern<vdpp::LoadOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::LoadOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        Type resType = getTypeConverter()->convertType(op.getResult().getType());
        if (!resType) return failure();
        rw.replaceOpWithNewOp<LLVM::LoadOp>(op, resType, adaptor.getPtr(),
                                            op.getAlignment());
        return success();
    }
};

struct StoreOpLowering : public OpConversionPattern<vdpp::StoreOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::StoreOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        rw.replaceOpWithNewOp<LLVM::StoreOp>(op, adaptor.getValue(),
                                             adaptor.getPtr(),
                                             op.getAlignment());
        return success();
    }
};

struct GEPOpLowering : public OpConversionPattern<vdpp::GetElementPtrOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::GetElementPtrOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        auto ptrType = LLVMPointerType::get(op.getContext());
        // GEP's elem_type is the BASE pointer's pointee type (the aggregate being
        // indexed into), NOT the result pointer's element type.
        auto basePtrTy = cast<vdpp::PointerType>(op.getBase().getType());
        Type elemLLVMTy = getTypeConverter()->convertType(basePtrTy.getElementType());
        if (!elemLLVMTy) return failure();
        SmallVector<LLVM::GEPArg> gepArgs;
        for (Value idx : adaptor.getIndices())
            gepArgs.push_back(idx);
        rw.replaceOpWithNewOp<LLVM::GEPOp>(op, ptrType, elemLLVMTy,
                                           adaptor.getBase(), gepArgs);
        return success();
    }
};

// ── Cast ────────────────────────────────────────────────────────────────────

struct CastOpLowering : public OpConversionPattern<vdpp::CastOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::CastOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        Type srcTy = op.getValue().getType();
        Type dstTy = op.getResult().getType();
        Type llvmDst = getTypeConverter()->convertType(dstTy);
        if (!llvmDst) return failure();

        Value src = adaptor.getValue();
        bool srcIsPtr = isa<vdpp::PointerType>(srcTy);
        bool dstIsPtr = isa<vdpp::PointerType>(dstTy);

        if (srcIsPtr && dstIsPtr) {
            // ptr → ptr: bitcast (no-op in opaque pointer world)
            rw.replaceOp(op, src);
            return success();
        }
        if (srcIsPtr) {
            // ptr → int
            rw.replaceOpWithNewOp<LLVM::PtrToIntOp>(op, llvmDst, src);
            return success();
        }
        if (dstIsPtr) {
            // int → ptr
            rw.replaceOpWithNewOp<LLVM::IntToPtrOp>(op, llvmDst, src);
            return success();
        }

        // int → int
        auto srcIntTy = cast<IntegerType>(srcTy);
        auto dstIntTy = cast<IntegerType>(dstTy);
        if (srcIntTy.getWidth() < dstIntTy.getWidth())
            rw.replaceOpWithNewOp<LLVM::ZExtOp>(op, llvmDst, src);
        else if (srcIntTy.getWidth() > dstIntTy.getWidth())
            rw.replaceOpWithNewOp<LLVM::TruncOp>(op, llvmDst, src);
        else
            rw.replaceOp(op, src); // same width, no-op
        return success();
    }
};

// ── Control Flow ─────────────────────────────────────────────────────────────

struct BranchOpLowering : public OpConversionPattern<vdpp::BranchOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::BranchOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        rw.replaceOpWithNewOp<LLVM::BrOp>(op, adaptor.getDestOperands(),
                                          op.getDest());
        return success();
    }
};

struct CondBranchOpLowering : public OpConversionPattern<vdpp::CondBranchOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(vdpp::CondBranchOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rw) const override {
        rw.replaceOpWithNewOp<LLVM::CondBrOp>(
            op, adaptor.getCondition(),
            op.getTrueDest(),  adaptor.getTrueDestOperands(),
            op.getFalseDest(), adaptor.getFalseDestOperands());
        return success();
    }
};

// ── Register all patterns ────────────────────────────────────────────────────

static void populateVDPPToLLVMPatterns(RewritePatternSet &patterns,
                                        VDPPTypeConverter &typeConverter) {
    MLIRContext *ctx = patterns.getContext();

    patterns.add<
        BinOpLowering<vdpp::AddOp,  LLVM::AddOp>,
        BinOpLowering<vdpp::SubOp,  LLVM::SubOp>,
        BinOpLowering<vdpp::MulOp,  LLVM::MulOp>,
        BinOpLowering<vdpp::DivOp,  LLVM::SDivOp>,
        BinOpLowering<vdpp::UDivOp, LLVM::UDivOp>,
        BinOpLowering<vdpp::AndOp,  LLVM::AndOp>,
        BinOpLowering<vdpp::OrOp,   LLVM::OrOp>,
        BinOpLowering<vdpp::XorOp,  LLVM::XOrOp>,
        ShiftOpLowering<vdpp::ShlOp,  LLVM::ShlOp>,
        ShiftOpLowering<vdpp::LShrOp, LLVM::LShrOp>,
        ShiftOpLowering<vdpp::AShrOp, LLVM::AShrOp>,
        ConstantOpLowering,
        ICmpOpLowering,
        AllocaOpLowering,
        LoadOpLowering,
        StoreOpLowering,
        GEPOpLowering,
        CastOpLowering,
        BranchOpLowering,
        CondBranchOpLowering
    >(typeConverter, ctx);

    // Standard func → llvm.func conversion (handles block args, call conv, etc.)
    populateFuncToLLVMConversionPatterns(typeConverter, patterns);
}

// ============================================================================
// Public API
// ============================================================================

namespace mlir {

LogicalResult emitVDPPAsLLVMIR(int blockId, StringRef vdppFilePath,
                                llvm::raw_ostream &os) {
    // Use a fresh, standalone MLIRContext so dialect loading and translation
    // registration are safe — this function is called inside a pass's
    // runOnOperation, where the pass manager's context is multi-threaded and
    // forbids loadDialect calls.
    MLIRContext freshCtx;
    freshCtx.loadDialect<vdpp::vDPPDialect>();
    freshCtx.loadDialect<func::FuncDialect>();
    freshCtx.loadDialect<arith::ArithDialect>();
    freshCtx.loadDialect<LLVM::LLVMDialect>();
    registerLLVMDialectTranslation(freshCtx);
    registerBuiltinDialectTranslation(freshCtx);

    auto mod = parseSourceFile<ModuleOp>(vdppFilePath, &freshCtx);
    if (!mod) {
        llvm::errs() << "  ✗ VDPPToLLVM: cannot parse " << vdppFilePath << "\n";
        return failure();
    }

    // Erase module-level vdpp.hash5tuple_instance declarations: they have no
    // LLVM IR equivalent and would otherwise be flagged as illegal ops.
    {
        llvm::SmallVector<mlir::Operation *> toErase;
        for (auto &op : *mod->getBody())
            if (mlir::isa<vdpp::Hash5TupleInstanceOp>(op))
                toErase.push_back(&op);
        for (auto *op : toErase) op->erase();
    }

    // Step 1: preprocess — fix function signature and vdpp.return.
    if (failed(preprocessModule(*mod, blockId)))
        return failure();

    // Step 2+3: convert vDPP ops → LLVM dialect (including func→llvm.func).
    VDPPTypeConverter typeConverter(&freshCtx);

    RewritePatternSet patterns(&freshCtx);
    populateVDPPToLLVMPatterns(patterns, typeConverter);

    LLVMConversionTarget target(freshCtx);
    target.addLegalOp<ModuleOp>();
    target.addIllegalDialect<vdpp::vDPPDialect>();
    target.addIllegalOp<func::FuncOp>();

    if (failed(applyPartialConversion(*mod, target, std::move(patterns)))) {
        llvm::errs() << "  ✗ VDPPToLLVM: dialect conversion failed for block "
                     << blockId << "\n";
        return failure();
    }

    // Step 4: translate LLVM dialect → llvm::Module and print as .ll.
    llvm::LLVMContext llvmCtx;
    auto llvmMod = translateModuleToLLVMIR(*mod, llvmCtx);
    if (!llvmMod) {
        llvm::errs() << "  ✗ VDPPToLLVM: LLVM IR translation failed for block "
                     << blockId << "\n";
        return failure();
    }

    llvmMod->print(os, nullptr);
    return success();
}

} // namespace mlir
