/**
 *  Copyright (c) 2022 by Contributors
 * @file softmax.cc
 * @brief DGL C++ Softmax operator implementation
 */

#include <sparse/reduction.h>
#include <sparse/sparse_matrix.h>
#include <torch/script.h>

#include "./matmul.h"
#include "./utils.h"

namespace dgl {
namespace sparse {

using namespace torch::autograd;

class SoftmaxAutoGrad : public Function<SoftmaxAutoGrad> {
 public:
  static torch::Tensor forward(
      AutogradContext* ctx, c10::intrusive_ptr<SparseMatrix> sparse_mat,
      torch::Tensor sparse_val);

  static tensor_list backward(AutogradContext* ctx, tensor_list grad_outputs);
};

torch::Tensor SoftmaxAutoGrad::forward(
    AutogradContext* ctx, c10::intrusive_ptr<SparseMatrix> sparse_mat,
    torch::Tensor sparse_val) {
  // Reduce by columns with dim 1.
  auto sparse_val_max = ReduceMax(sparse_mat, 1);
  auto sparse_val_exp =
      BroadcastSubNoAutoGrad(sparse_mat, sparse_val_max).exp();
  auto sparse_val_sum = ReduceSum(CreateValLike(sparse_mat, sparse_val_exp), 1);
  auto sparse_score = BroadcastDivNoAutoGrad(
      CreateValLike(sparse_mat, sparse_val_exp), sparse_val_sum);

  const bool sparse_requires_grad = sparse_val.requires_grad();
  torch::Tensor cache_sparse_score;
  if (sparse_requires_grad) {
    cache_sparse_score = sparse_score;
  }
  ctx->saved_data["sparse_matrix"] = sparse_mat;
  ctx->saved_data["sparse_requires_grad"] = sparse_requires_grad;
  ctx->save_for_backward({cache_sparse_score});
  return sparse_score;
}

tensor_list SoftmaxAutoGrad::backward(
    AutogradContext* ctx, tensor_list grad_outputs) {
  auto saved = ctx->get_saved_variables();
  auto sparse_score = saved[0];
  auto output_grad = grad_outputs[0];

  auto sparse_mat =
      ctx->saved_data["sparse_matrix"].toCustomClass<SparseMatrix>();
  const bool sparse_requires_grad =
      ctx->saved_data["sparse_requires_grad"].toBool();

  torch::Tensor sparse_val_grad;
  if (sparse_requires_grad) {
    auto sds = sparse_score * output_grad;
    auto accum = ReduceSum(CreateValLike(sparse_mat, sds), 1);
    sparse_val_grad = sds - BroadcastMulNoAutoGrad(
                                CreateValLike(sparse_mat, sparse_score), accum);
  }

  return {torch::Tensor(), sparse_val_grad};
}

c10::intrusive_ptr<SparseMatrix> Softmax(
    const c10::intrusive_ptr<SparseMatrix>& sparse_mat) {
  auto sparse_val = sparse_mat->value();
  bool expand_dim = false;
  auto new_sparse_mat = sparse_mat;
  if (sparse_val.dim() == 1) {
    sparse_val = sparse_val.view({-1, 1});
    expand_dim = true;
    new_sparse_mat = CreateValLike(sparse_mat, sparse_val);
  }

  auto new_sparse_val = SoftmaxAutoGrad::apply(new_sparse_mat, sparse_val);

  if (expand_dim) {
    new_sparse_val = new_sparse_val.view(-1);
  }
  return CreateValLike(sparse_mat, new_sparse_val);
}

}  // namespace sparse
}  // namespace dgl
