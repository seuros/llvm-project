//===------ WmmaOpsToNVVM.cpp - WMMA LD/ST/Compute to NVVM lowering -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions of patterns to lower GPU Subgroup MMA ops to
// NVVM Dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/IR/TypeUtilities.h"

using namespace mlir;

namespace {

/// Checks if all the operands of the op being lowered are of LLVM Types. The
/// types are expected to be converted by the `LLVMTypeConverter` before the op
/// is actually lowered. If the type of an operands is not already converted it
/// hints a missing typeConversion and failure is returned in that case.
static LogicalResult areAllLLVMTypes(Operation *op, ValueRange operands,
                                     ConversionPatternRewriter &rewriter) {
  if (!llvm::all_of(operands, [](Value value) {
        return LLVM::isCompatibleType(value.getType());
      })) {
    return rewriter.notifyMatchFailure(
        op, "cannot convert if operands aren't of LLVM type.");
  }

  return success();
}

/// Error string to emit when an unimplemented WMMA variant is encountered.
static constexpr StringRef kInvalidCaseStr = "Unsupported WMMA variant.";

static NVVM::MMAFrag convertOperand(StringRef operandName) {
  if (operandName == "AOp")
    return NVVM::MMAFrag::a;
  if (operandName == "BOp")
    return NVVM::MMAFrag::b;
  if (operandName == "COp")
    return NVVM::MMAFrag::c;
  llvm_unreachable("Unknown operand name");
}

static NVVM::MMATypes getElementType(gpu::MMAMatrixType type) {
  if (type.getElementType().isF16())
    return NVVM::MMATypes::f16;
  if (type.getElementType().isF32())
    return type.getOperand() == "COp" ? NVVM::MMATypes::f32
                                      : NVVM::MMATypes::tf32;

  if (type.getElementType().isSignedInteger(8))
    return NVVM::MMATypes::s8;
  if (type.getElementType().isUnsignedInteger(8))
    return NVVM::MMATypes::u8;
  // Accumulator type is signless and implies signed.
  if (type.getElementType().isInteger(32))
    return NVVM::MMATypes::s32;
  llvm_unreachable("Unsupported type");
}

/// This class implements the conversion of GPU MMA loadOp to wmma.load op
/// in the NVVM dialect. The conversion not only emits the NVVM op but also
/// emits code that is necessary to store the data in the destination memref
/// after it has been loaded.
struct WmmaLoadOpToNVVMLowering
    : public ConvertOpToLLVMPattern<gpu::SubgroupMmaLoadMatrixOp> {
  using ConvertOpToLLVMPattern<
      gpu::SubgroupMmaLoadMatrixOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaLoadMatrixOp subgroupMmaLoadMatrixOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Operation *op = subgroupMmaLoadMatrixOp.getOperation();
    if (failed(areAllLLVMTypes(op, adaptor.getOperands(), rewriter)))
      return failure();

    // Get the shape of the MMAMatrix type being returned. The shape will
    // choose which intrinsic this op will be lowered to.
    NVVM::MMALayout layout = subgroupMmaLoadMatrixOp.getTranspose()
                                 ? NVVM::MMALayout::col
                                 : NVVM::MMALayout::row;
    gpu::MMAMatrixType retType =
        cast<gpu::MMAMatrixType>(subgroupMmaLoadMatrixOp.getRes().getType());
    ArrayRef<int64_t> retTypeShape = retType.getShape();
    int64_t m = 0;
    int64_t n = 0;
    int64_t k = 0;
    NVVM::MMATypes eltype = getElementType(retType);
    // NVVM intrinsics require to give mxnxk dimensions, infer the missing
    // dimension based on the valid intrinsics available.
    if (retType.getOperand() == "AOp") {
      m = retTypeShape[0];
      k = retTypeShape[1];
      n = NVVM::WMMALoadOp::inferNDimension(m, k, eltype);
    } else if (retType.getOperand() == "BOp") {
      k = retTypeShape[0];
      n = retTypeShape[1];
      m = NVVM::WMMALoadOp::inferMDimension(k, n, eltype);
    } else if (retType.getOperand() == "COp") {
      m = retTypeShape[0];
      n = retTypeShape[1];
      k = NVVM::WMMALoadOp::inferKDimension(m, n, eltype);
    }
    NVVM::MMAFrag frag = convertOperand(retType.getOperand());
    // Check that there is an exisiting instruction for the combination we need.
    if (NVVM::WMMALoadOp::getIntrinsicID(m, n, k, layout, eltype, frag) == 0)
      return rewriter.notifyMatchFailure(op, kInvalidCaseStr);

    Type resType = convertMMAToLLVMType(retType);
    Location loc = op->getLoc();

    // Create nvvm.mma_load op according to the operand types.
    Value dataPtr = getStridedElementPtr(
        rewriter, loc,
        cast<MemRefType>(subgroupMmaLoadMatrixOp.getSrcMemref().getType()),
        adaptor.getSrcMemref(), adaptor.getIndices());

    Value leadingDim = LLVM::ConstantOp::create(
        rewriter, loc, rewriter.getI32Type(),
        subgroupMmaLoadMatrixOp.getLeadDimensionAttr());
    rewriter.replaceOpWithNewOp<NVVM::WMMALoadOp>(
        op, resType, dataPtr, leadingDim, m, n, k, layout, eltype, frag);
    return success();
  }
};

/// This class implements the conversion of GPU MMA storeOp to wmma.store op
/// in the NVVM dialect. The conversion not only emits the NVVM op but also
/// emits code that is necessary to unpack the data in the source and
/// convert the data in the format that is needed by the NVVM op.
struct WmmaStoreOpToNVVMLowering
    : public ConvertOpToLLVMPattern<gpu::SubgroupMmaStoreMatrixOp> {
  using ConvertOpToLLVMPattern<
      gpu::SubgroupMmaStoreMatrixOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaStoreMatrixOp subgroupMmaStoreMatrixOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Operation *op = subgroupMmaStoreMatrixOp.getOperation();
    if (failed(areAllLLVMTypes(op, adaptor.getOperands(), rewriter)))
      return failure();

    Location loc = op->getLoc();

    SmallVector<Value, 4> storeOpOperands;
    // Get the shape of the MMAMatrix type being stored. The shape will
    // choose which intrinsic this op will be lowered to.
    gpu::MMAMatrixType srcType =
        cast<gpu::MMAMatrixType>(subgroupMmaStoreMatrixOp.getSrc().getType());
    ArrayRef<int64_t> srcTypeShape = srcType.getShape();
    NVVM::MMALayout layout = subgroupMmaStoreMatrixOp.getTranspose()
                                 ? NVVM::MMALayout::col
                                 : NVVM::MMALayout::row;
    NVVM::MMATypes eltype = getElementType(srcType);
    int64_t m = srcTypeShape[0];
    int64_t n = srcTypeShape[1];
    int64_t k = NVVM::WMMAStoreOp::inferKDimension(m, n, eltype);
    if (NVVM::WMMAStoreOp::getIntrinsicID(m, n, k, layout, eltype) == 0)
      return rewriter.notifyMatchFailure(op, kInvalidCaseStr);

    auto matrixType = cast<LLVM::LLVMStructType>(adaptor.getSrc().getType());
    for (unsigned i = 0, e = matrixType.getBody().size(); i < e; ++i) {
      Value toUse =
          LLVM::ExtractValueOp::create(rewriter, loc, adaptor.getSrc(), i);
      storeOpOperands.push_back(toUse);
    }

    Value dataPtr = getStridedElementPtr(
        rewriter, loc,
        cast<MemRefType>(subgroupMmaStoreMatrixOp.getDstMemref().getType()),
        adaptor.getDstMemref(), adaptor.getIndices());
    Value leadingDim = LLVM::ConstantOp::create(
        rewriter, loc, rewriter.getI32Type(),
        subgroupMmaStoreMatrixOp.getLeadDimensionAttr());
    rewriter.replaceOpWithNewOp<NVVM::WMMAStoreOp>(
        op, dataPtr, m, n, k, layout, eltype, storeOpOperands, leadingDim);
    return success();
  }
};

/// This class implements the conversion of GPU MMA computeOp to wmma.mma op
/// in the NVVM dialect.
struct WmmaMmaOpToNVVMLowering
    : public ConvertOpToLLVMPattern<gpu::SubgroupMmaComputeOp> {
  using ConvertOpToLLVMPattern<
      gpu::SubgroupMmaComputeOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaComputeOp subgroupMmaComputeOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Operation *op = subgroupMmaComputeOp.getOperation();
    if (failed(areAllLLVMTypes(op, adaptor.getOperands(), rewriter)))
      return failure();

    Location loc = op->getLoc();

    // The wmma.mma intrinsic in llvm requires the operands as individual
    // values. So individual elements from the memrefs need to be extracted and
    // then passed on to the intrinsic call. Emit llvm ops to extract individual
    // values form lowered memrefs.
    SmallVector<Value> unpackedOps;

    auto unpackOp = [&](Value operand) {
      auto structType = cast<LLVM::LLVMStructType>(operand.getType());
      for (size_t i = 0, e = structType.getBody().size(); i < e; ++i) {
        Value toUse = LLVM::ExtractValueOp::create(rewriter, loc, operand, i);
        unpackedOps.push_back(toUse);
      }
    };

    // Get the shapes of the MMAMatrix type being used. The shapes will
    // choose which intrinsic this op will be lowered to.
    gpu::MMAMatrixType aType =
        cast<gpu::MMAMatrixType>(subgroupMmaComputeOp.getOpA().getType());
    ArrayRef<int64_t> aTypeShape = aType.getShape();
    gpu::MMAMatrixType cType =
        cast<gpu::MMAMatrixType>(subgroupMmaComputeOp.getOpC().getType());
    ArrayRef<int64_t> cTypeShape = cType.getShape();
    int64_t m = cTypeShape[0];
    int64_t n = cTypeShape[1];
    int64_t k = aTypeShape[1];
    NVVM::MMALayout aLayout = subgroupMmaComputeOp.getATranspose()
                                  ? NVVM::MMALayout::col
                                  : NVVM::MMALayout::row;
    NVVM::MMALayout bLayout = subgroupMmaComputeOp.getBTranspose()
                                  ? NVVM::MMALayout::col
                                  : NVVM::MMALayout::row;
    NVVM::MMATypes sourceType = getElementType(aType);
    NVVM::MMATypes destType = getElementType(cType);
    if (NVVM::WMMAMmaOp::getIntrinsicID(m, n, k, aLayout, bLayout, sourceType,
                                        destType) == 0)
      return rewriter.notifyMatchFailure(op, kInvalidCaseStr);

    NVVM::MMATypes bElementType = getElementType(
        cast<gpu::MMAMatrixType>(subgroupMmaComputeOp.getOpB().getType()));
    if (bElementType != sourceType)
      return rewriter.notifyMatchFailure(
          op, "WMMA compute op input matrix element types must match.");

    unpackOp(adaptor.getOpA());
    unpackOp(adaptor.getOpB());
    unpackOp(adaptor.getOpC());

    rewriter.replaceOpWithNewOp<NVVM::WMMAMmaOp>(
        op, adaptor.getOpC().getType(), m, n, k, aLayout, bLayout, sourceType,
        destType, unpackedOps);
    return success();
  }
};

/// Convert GPU MMA ConstantMatrixOp to a chain of InsertValueOp.
struct WmmaConstantOpToNVVMLowering
    : public ConvertOpToLLVMPattern<gpu::SubgroupMmaConstantMatrixOp> {
  using ConvertOpToLLVMPattern<
      gpu::SubgroupMmaConstantMatrixOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaConstantMatrixOp subgroupMmaConstantOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (failed(areAllLLVMTypes(subgroupMmaConstantOp.getOperation(),
                               adaptor.getOperands(), rewriter)))
      return failure();
    Location loc = subgroupMmaConstantOp.getLoc();
    Value cst = adaptor.getOperands()[0];
    LLVM::LLVMStructType type = convertMMAToLLVMType(
        cast<gpu::MMAMatrixType>(subgroupMmaConstantOp.getType()));
    // If the element type is a vector create a vector from the operand.
    if (auto vecType = dyn_cast<VectorType>(type.getBody()[0])) {
      Value vecCst = LLVM::PoisonOp::create(rewriter, loc, vecType);
      for (int64_t vecEl = 0; vecEl < vecType.getNumElements(); vecEl++) {
        Value idx = LLVM::ConstantOp::create(rewriter, loc,
                                             rewriter.getI32Type(), vecEl);
        vecCst = LLVM::InsertElementOp::create(rewriter, loc, vecType, vecCst,
                                               cst, idx);
      }
      cst = vecCst;
    }
    Value matrixStruct = LLVM::PoisonOp::create(rewriter, loc, type);
    for (size_t i : llvm::seq(size_t(0), type.getBody().size())) {
      matrixStruct =
          LLVM::InsertValueOp::create(rewriter, loc, matrixStruct, cst, i);
    }
    rewriter.replaceOp(subgroupMmaConstantOp, matrixStruct);
    return success();
  }
};

static Value createMinMaxF(OpBuilder &builder, Location loc, Value lhs,
                           Value rhs, bool isMin) {
  auto floatType = cast<FloatType>(getElementTypeOrSelf(lhs.getType()));
  Type i1Type = builder.getI1Type();
  if (auto vecType = dyn_cast<VectorType>(lhs.getType()))
    i1Type = VectorType::get(vecType.getShape(), i1Type);
  Value cmp = LLVM::FCmpOp::create(
      builder, loc, i1Type,
      isMin ? LLVM::FCmpPredicate::olt : LLVM::FCmpPredicate::ogt, lhs, rhs);
  Value sel = LLVM::SelectOp::create(builder, loc, cmp, lhs, rhs);
  Value isNan = LLVM::FCmpOp::create(builder, loc, i1Type,
                                     LLVM::FCmpPredicate::uno, lhs, rhs);
  Value nan = LLVM::ConstantOp::create(
      builder, loc, lhs.getType(),
      builder.getFloatAttr(floatType,
                           APFloat::getQNaN(floatType.getFloatSemantics())));
  return LLVM::SelectOp::create(builder, loc, isNan, nan, sel);
}

static Value createScalarOp(OpBuilder &builder, Location loc,
                            gpu::MMAElementwiseOp op,
                            ArrayRef<Value> operands) {
  switch (op) {
  case gpu::MMAElementwiseOp::ADDF:
    return LLVM::FAddOp::create(builder, loc, operands[0].getType(), operands);
  case gpu::MMAElementwiseOp::MULF:
    return LLVM::FMulOp::create(builder, loc, operands[0].getType(), operands);
  case gpu::MMAElementwiseOp::DIVF:
    return LLVM::FDivOp::create(builder, loc, operands[0].getType(), operands);
  case gpu::MMAElementwiseOp::MAXF:
    return createMinMaxF(builder, loc, operands[0], operands[1],
                         /*isMin=*/false);
  case gpu::MMAElementwiseOp::MINF:
    return createMinMaxF(builder, loc, operands[0], operands[1],
                         /*isMin=*/true);
  default:
    llvm_unreachable("unknown op");
  }
}

/// Convert GPU MMA elementwise ops to extract + op + insert.
struct WmmaElementwiseOpToNVVMLowering
    : public ConvertOpToLLVMPattern<gpu::SubgroupMmaElementwiseOp> {
  using ConvertOpToLLVMPattern<
      gpu::SubgroupMmaElementwiseOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaElementwiseOp subgroupMmaElementwiseOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (failed(areAllLLVMTypes(subgroupMmaElementwiseOp.getOperation(),
                               adaptor.getOperands(), rewriter)))
      return failure();
    Location loc = subgroupMmaElementwiseOp.getLoc();
    size_t numOperands = adaptor.getOperands().size();
    LLVM::LLVMStructType destType = convertMMAToLLVMType(
        cast<gpu::MMAMatrixType>(subgroupMmaElementwiseOp.getType()));
    Value matrixStruct = LLVM::PoisonOp::create(rewriter, loc, destType);
    for (size_t i = 0, e = destType.getBody().size(); i < e; ++i) {
      SmallVector<Value> extractedOperands;
      for (size_t opIdx = 0; opIdx < numOperands; opIdx++) {
        extractedOperands.push_back(LLVM::ExtractValueOp::create(
            rewriter, loc, adaptor.getOperands()[opIdx], i));
      }
      Value element =
          createScalarOp(rewriter, loc, subgroupMmaElementwiseOp.getOpType(),
                         extractedOperands);
      matrixStruct =
          LLVM::InsertValueOp::create(rewriter, loc, matrixStruct, element, i);
    }
    rewriter.replaceOp(subgroupMmaElementwiseOp, matrixStruct);
    return success();
  }
};

} // namespace

/// Return the LLVMStructureType corresponding to the MMAMatrixType `type`.
LLVM::LLVMStructType mlir::convertMMAToLLVMType(gpu::MMAMatrixType type) {
  NVVM::MMAFrag frag = convertOperand(type.getOperand());
  NVVM::MMATypes eltType = getElementType(type);
  auto nRow = type.getShape()[0];
  auto nCol = type.getShape()[1];
  std::pair<Type, unsigned> typeInfo =
      NVVM::inferMMAType(eltType, frag, nRow, nCol, type.getContext());
  return LLVM::LLVMStructType::getLiteral(
      type.getContext(), SmallVector<Type, 8>(typeInfo.second, typeInfo.first));
}

void mlir::populateGpuWMMAToNVVMConversionPatterns(
    const LLVMTypeConverter &converter, RewritePatternSet &patterns,
    PatternBenefit benefit) {
  patterns.add<WmmaLoadOpToNVVMLowering, WmmaMmaOpToNVVMLowering,
               WmmaStoreOpToNVVMLowering, WmmaConstantOpToNVVMLowering,
               WmmaElementwiseOpToNVVMLowering>(converter, benefit);
}
