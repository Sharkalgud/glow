/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FusePrepack.h"

#include <torch/csrc/jit/passes/alias_analysis.h>
#include <torch/csrc/jit/passes/subgraph_rewrite.h>

namespace glow {

/// This pass fuse the quantized::conv_prepack + quantized::conv2d generated by
/// JIT back to quantized::unpacked_conv2d since we dont have
/// quantized::conv_prepack in glow. However regular packed conv's
/// implementation in glow is still needed.
void FuseConvPrepack(std::shared_ptr<torch::jit::Graph> &graph) {
  std::string convPrepackPattern = R"IR(
graph(%input, %w, %b, %4, %5, %6, %7, %8, %9, %10, %11, %12):
  %prepacked_weight = quantized::conv_prepacked(%w, %b, %4, %5, %6, %7)
  %res = quantized::conv2d(%input, %prepacked_weight, %8, %9, %10, %7, %11, %12)
  return (%res))IR";

  std::string convFused = R"IR(
graph(%input, %w, %b, %4, %5, %6, %7, %8, %9, %10, %11, %12):
  %res = quantized::unpacked_conv2d(%input, %w, %b, %8, %9, %10, %7, %11, %12)
  return (%res))IR";

  // Replace conv_prepack + conv2d to unpacked_conv2d
  torch::jit::SubgraphRewriter convToUnpackedConv;
  convToUnpackedConv.RegisterRewritePattern(convPrepackPattern, convFused);
  convToUnpackedConv.runOnGraph(graph);
}
} // namespace glow
