// Copyright 2023 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <iree/compiler/Codegen/Dialect/IREECodegenAttrs.h>
#include "iree/compiler/Dialect/Flow/Transforms/RegionOpUtils.h"
#include "iree/compiler/Preprocessing/Common/PassDetail.h"
#include "iree/compiler/Preprocessing/Common/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define DEBUG_TYPE "iree-preprocessing-rematerialize-parallel-ops"

namespace mlir {
namespace iree_compiler {
namespace IREE {

namespace {

static bool isScalarOrTensorOfSizeOne(Type t) {
  if (auto tensorType = dyn_cast<RankedTensorType>(t)) {
    return tensorType.hasStaticShape() && tensorType.getNumElements() == 1;
  }
  return t.isIntOrIndexOrFloat();
}

/// Rematerialize all parallel elementwise operations into its users within a
/// `flow.dispatch.region`.
struct RematerializeParallelOpsPattern
    : public OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(linalg::GenericOp genericOp,
                                PatternRewriter &rewriter) const override {
    // Avoid doing this for scalar operations.
    auto isScalarValue = [](Value v) {
      return isScalarOrTensorOfSizeOne(v.getType());
    };
    if (llvm::all_of(genericOp.getOperands(), isScalarValue) &&
        llvm::all_of(genericOp.getResults(), isScalarValue)) {
      return failure();
    }

    // Find the first operand that is defined by another generic op on tensors.
    for (OpOperand &opOperand : genericOp->getOpOperands()) {
      if (!linalg::areElementwiseOpsFusable(&opOperand))
        continue;

      FailureOr<linalg::ElementwiseOpFusionResult> fusionResult =
          linalg::fuseElementwiseOps(rewriter, &opOperand);
      if (succeeded(fusionResult)) {
        if (Attribute attr = genericOp->getAttr("lowering_config")) {
          fusionResult->fusedOp->setAttr("lowering_config", attr);
        }
        auto replacements = fusionResult->fusedOp->getResults().take_back(
            genericOp.getNumResults());
        rewriter.replaceOp(genericOp, replacements);
        return success();
      }
    }
    return failure();
  }
};

class RematerializeParallelOpsPass
    : public RematerializeParallelOpsBase<RematerializeParallelOpsPass> {
public:
  RematerializeParallelOpsPass(std::function<bool(func::FuncOp)> controlFn)
      : controlFn(controlFn) {}
  void runOnOperation() override {
    func::FuncOp funcOp = getOperation();
    if (controlFn && !controlFn(funcOp))
      return;

    RewritePatternSet fusionPatterns(funcOp.getContext());
    fusionPatterns.insert<RematerializeParallelOpsPattern>(funcOp.getContext());
    linalg::populateEraseUnusedOperandsAndResultsPatterns(fusionPatterns);
    if (failed(
            applyPatternsAndFoldGreedily(funcOp, std::move(fusionPatterns)))) {
      return signalPassFailure();
    }
  }

private:
  std::function<bool(func::FuncOp)> controlFn;
};

} // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createRematerializeParallelOpsPass(std::function<bool(func::FuncOp)> controlFn) {
  return std::make_unique<RematerializeParallelOpsPass>(controlFn);
}

} // namespace IREE
} // namespace iree_compiler
} // namespace mlir
