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
#include "BackendTestUtils.h"

#include "glow/Converter/TypeAToTypeBFunctionConverter.h"
#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/Graph/Graph.h"
#include "glow/Partitioner/Partitioner.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <random>

#include "gtest/gtest.h"

#include "llvm/Support/CommandLine.h"

constexpr size_t MAX_MEMORY = 64e+9;

using namespace glow;

namespace {
llvm::cl::OptionCategory recSysTestCat("RecSys Category");

llvm::cl::opt<unsigned> miniBatchOpt("mini-batch", llvm::cl::desc("Minibatch."),
                                     llvm::cl::Optional, llvm::cl::init(8),
                                     llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> concurrentReqestsOpt(
    "concurrent-count", llvm::cl::desc("Number of concurrent requests."),
    llvm::cl::Optional, llvm::cl::init(1), llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> embeddingDimOpt("embedding-dim",
                                        llvm::cl::desc("Embedding dim."),
                                        llvm::cl::Optional, llvm::cl::init(64),
                                        llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> denseDimOpt("dense-dim", llvm::cl::desc("Dense dim."),
                                    llvm::cl::Optional, llvm::cl::init(800),
                                    llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> numHiddenBottomMLPLayersOpt(
    "num-hidden-bottom-mlp-layers",
    llvm::cl::desc("Number of hidden bottom MLP layers."), llvm::cl::Optional,
    llvm::cl::init(3), llvm::cl::cat(recSysTestCat));

llvm::cl::list<unsigned> bottomMLPIntermediateDimsOpt(
    "bottom-mlp-intermediate-dims",
    llvm::cl::desc(
        "Comma-separated list of intermediate dim for each of the bottom MLP "
        "hidden layers and output layer. Will wrap around to the start of the "
        "list and reuse dimensions if less than the number of layers. If "
        "unprovided, default is 1024."),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated,
    llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned>
    numHiddenTopMLPLayersOpt("num-hidden-top-mlp-layers",
                             llvm::cl::desc("Number of hidden top MLP layers."),
                             llvm::cl::Optional, llvm::cl::init(3),
                             llvm::cl::cat(recSysTestCat));

llvm::cl::list<unsigned> topMLPIntermediateDimsOpt(
    "top-mlp-intermediate-dims",
    llvm::cl::desc(
        "Comma-separated list of intermediate dim for each of the top MLP "
        "hidden layers and output layer. Will wrap around to the start of the "
        "list and reuse dimensions if less than the number of layers. If "
        "unprovided, default is 1024."),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated,
    llvm::cl::cat(recSysTestCat));

llvm::cl::list<unsigned> lengthsMinMaxOpt(
    "lengths-min-max",
    llvm::cl::desc("Comma separated [min, max) value to be used when "
                   "generating random lengths inputs for SLS/SLWS. If left "
                   "unspecified, will use [90, 110)."),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated,
    llvm::cl::cat(recSysTestCat));

llvm::cl::list<unsigned> tableSizesOpt(
    "embedding-table-sizes",
    llvm::cl::desc("Comma-separated list of embedding table sizes."),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated,
    llvm::cl::cat(recSysTestCat));

llvm::cl::list<unsigned> tableCountsOpt(
    "embedding-table-counts",
    llvm::cl::desc("Comma-separated list of embedding table counts, "
                   "corresponding to a count for each size listed in "
                   "embedding-table-sizes."),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated,
    llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> deviceMemCapacityOpt(
    "device-mem-capacity",
    llvm::cl::desc("Device memory capacity in kB. Default is dependent on the "
                   "test in order to potentially force partitioning."),
    llvm::cl::Optional, llvm::cl::init(0), llvm::cl::cat(recSysTestCat));

llvm::cl::opt<unsigned> numDevicesOpt(
    "num-devices", llvm::cl::desc("Number of devices to use for partitioning."),
    llvm::cl::Optional, llvm::cl::init(6), llvm::cl::cat(recSysTestCat));

llvm::cl::opt<bool> useSymmetricRowwiseQuantFCOpt(
    "use-symmetric-rowwise-quant-fc",
    llvm::cl::desc(
        "Whether to use Symmetric quantization with FCs. Default is false."),
    llvm::cl::Optional, llvm::cl::init(false), llvm::cl::cat(recSysTestCat));

llvm::cl::opt<std::string> traceDir(
    "trace-dir",
    llvm::cl::desc("Directory used to store Glow trace events files. If not "
                   "used, tracing is not enabled."),
    llvm::cl::Optional, llvm::cl::cat(recSysTestCat));
} // namespace

/// Fills the tensor \p H with some stable random data with the seed \p seed
/// and the range [-scale .. scale].
static void fillStableRandomData(Handle<float> H, size_t seed,
                                 float scale = 1) {
  for (size_t i = 0, e = H.size(); i < e; i++) {
    H.raw(i) = scale * (float((int(i * 1921 + seed) % 100) - 50) / 50);
  }
}

/// Fills the tensor \p H with some stable random integers with the seed \p
/// seed and the range [0, scale).
template <typename T>
static void fillStableRandomIndex(Handle<T> H, size_t seed, size_t min = 0,
                                  size_t max = 10) {
  for (size_t i = 0, e = H.size(); i < e; i++) {
    H.raw(i) = min + (int(i * 1921 + seed) % (max - min));
  }
}
template void fillStableRandomIndex(Handle<int64_t> Handle, size_t seed,
                                    size_t min, size_t max);
template void fillStableRandomIndex(Handle<int32_t> Handle, size_t seed,
                                    size_t min, size_t max);

/// Sum of all elements in Tensor.
static size_t sumOfElements(Handle<int32_t> H) {
  size_t sum = 0;
  for (size_t i = 0, e = H.size(); i < e; i++) {
    sum += H.raw(i);
  }
  return sum;
}

/// Helper method to wrap dispatching an inference request to a Hostmanager as
/// a synchronous interface. If concurrentReqestsOpt is set it will duplicate
/// the request to send multiple requests concurrently.
static void dispatchInference(HostManager *hostManager,
                              ExecutionContext &context) {
  // If additional requests are desired, setup additional contexts.
  std::vector<std::unique_ptr<ExecutionContext>> contexts;
  std::unique_ptr<ExecutionContext> originalContextPtr(&context);
  contexts.push_back(std::move(originalContextPtr));
  if (concurrentReqestsOpt > 1) {
    // Clone the placeholder bindings into a new executionContext.
    for (unsigned i = 0, max = concurrentReqestsOpt - 1; i < max; i++) {
      std::unique_ptr<ExecutionContext> newContext =
          llvm::make_unique<ExecutionContext>(
              llvm::make_unique<PlaceholderBindings>(
                  context.getPlaceholderBindings()->clone()));
      contexts.push_back(std::move(newContext));
    }
  }
  std::vector<std::promise<void>> promises(concurrentReqestsOpt);
  std::vector<std::future<void>> futures;
  for (auto &promise : promises) {
    futures.push_back(promise.get_future());
  }
  for (unsigned i = 0; i < concurrentReqestsOpt; i++) {
    hostManager->runNetwork("main", std::move(contexts[i]),
                            [&contexts, &promises,
                             i](runtime::RunIdentifierTy, Error err,
                                std::unique_ptr<ExecutionContext> contextPtr) {
                              contexts[i] = std::move(contextPtr);
                              // Expect no errors.
                              EXIT_ON_ERR(std::move(err));
                              promises[i].set_value();
                            });
  }

  for (auto &future : futures) {
    future.wait();
  }
  // Release the original context passed in by reference so we don't free it.
  contexts[0].release();
}

/// Helper function to generate deviceConfigs for HostManager initialization.
static std::vector<std::unique_ptr<runtime::DeviceConfig>>
generateDeviceConfigs(llvm::StringRef backendName, unsigned numDevices,
                      size_t memorySize) {
  std::vector<std::unique_ptr<runtime::DeviceConfig>> configs;
  for (unsigned i = 0; i < numDevices; i++) {
    auto deviceConfig = llvm::make_unique<DeviceConfig>(backendName);
    deviceConfig->setDeviceMemory(memorySize);
    configs.push_back(std::move(deviceConfig));
  }
  return configs;
}

/// Tests a simplified Recommendation System model.
///
/// The RecSys model has four components:
///    * An initial Multilayer Perceptron acting in the inputs.
///    * Some number of Sparse Features: SparseLengthSum nodes acting on
///      embedding tables (see https://caffe2.ai/docs/sparse-operations.html).
///    * An interaction layer bringing together the output for hte top MLP and
///      the sparse features.
///    * A final MLP acting on the result of the interaction.
///
/// The final result is a float indicating the strength of the recommendation.
///
///
///              +------+
///              |Output|
///              +--^---+
///                 |
///             +---+---+
///             |  TOP  |
///             |       |
///             |  MLP  |
///             +---^---+
///                 |
///                 |
///         +-------+--------+
///         |   Interaction  <---------+
///   +----->                <---+     |
///   |     +--------^-----^-+   |     |
///   |              |     |     |     |
/// +--+----+      +-+-+ +-+-+ +-+-+ +-+-+
/// | Bottom|      |SLS| |SLS| |SLS| |SLS|
/// |       |      +---+ +---+ +---+ +---+
/// |  MLP  |          Sparse Features
/// +---^---+
///     |
/// +---+---+
/// | Input |
/// +-------+
///
class RecommendationSystemTest : public BackendTest {
public:
  RecommendationSystemTest() : BackendTest(/* deviceMemory */ MAX_MEMORY) {}

protected:
  ExecutionContext context_;
  PlaceholderBindings *bindings_;
  PrecisionConfiguration precConfig_;

  // Test Config:
  size_t miniBatch;
  size_t embeddingDim;
  size_t denseDim;
  std::vector<size_t> tableSizes;
  std::vector<size_t> bottomMLPIntermediateDims;
  std::vector<size_t> topMLPIntermediateDims;
  size_t lengthsMin;
  size_t lengthsMax;

  // Used to configure correct precision settings:
  bool quantizeSLWSData{false};
  bool quantizeFC{false};
  bool convertToFP16{false};
  bool useFP16SLWS{false};
  bool useFP16AccumSLWS{false};

  // Whether to use SLWS with gather of weights, instead of SLS.
  bool gatherWeights{false};

  // Partitioner config:
  uint64_t deviceMemCapacity;
  size_t numDevices;

  // Result from executing the unpartitioned model on the backend being tested.
  Tensor *resultTensor{nullptr};

  /// Helper that \returns intermediate dims given a provided list of dims \p
  /// providedIntermediateDims and the number of layers needed \p numLayers. If
  /// the provided list is empty then all dims will be set to
  /// \p defaultIntermediateDim. If the size of \p providedIntermediateDims is
  /// less than \p numLayers then it will wrap around and reuse
  /// \p providedIntermediateDims until \p numLayers are added to the returned
  /// vector.
  static std::vector<size_t>
  getIntermediateDims(llvm::ArrayRef<unsigned> providedIntermediateDims,
                      unsigned numLayers,
                      size_t defaultIntermediateDim = 1024) {
    std::vector<size_t> destIntermediateDims;
    std::vector<size_t> dims(providedIntermediateDims.begin(),
                             providedIntermediateDims.end());
    if (dims.empty()) {
      dims.push_back(defaultIntermediateDim);
    }
    const size_t numProvidedDimsTop = dims.size();
    // Note: Add one extra intermediate dim, which is used by the output layer
    // of the MLP. The input layer is set based on its own input.
    for (size_t i = 0, e = numLayers + 1; i < e; i++) {
      destIntermediateDims.push_back(dims[i % numProvidedDimsTop]);
    }
    return destIntermediateDims;
  }

  void SetUp() override {
    bindings_ = context_.getPlaceholderBindings();

    /// Test configuration, tweak here:
    miniBatch = miniBatchOpt;
    embeddingDim = embeddingDimOpt;
    denseDim = denseDimOpt;
    lengthsMin = 90;
    lengthsMax = 111;

    if (!tableSizesOpt.empty()) {
      if (!tableCountsOpt.empty()) {
        CHECK_EQ(tableSizesOpt.size(), tableCountsOpt.size())
            << "Embedding table sizes and counts must be same length.";
        for (size_t i = 0, e = tableSizesOpt.size(); i < e; i++) {
          for (size_t j = 0, f = tableCountsOpt[i]; j < f; j++) {
            tableSizes.push_back(tableSizesOpt[i]);
          }
        }
      } else {
        tableSizes =
            std::vector<size_t>(tableSizesOpt.begin(), tableSizesOpt.end());
      }
      // Stable randomization of the order of the tables.
      std::shuffle(tableSizes.begin(), tableSizes.end(), std::mt19937());
    } else {
      tableSizes = {8000, 6000, 7000, 9000, 12000,
                    8000, 6000, 7000, 9000, 12000};
    }

    // Set up the bottom and top MLP intermediate dimensions.
    bottomMLPIntermediateDims = getIntermediateDims(
        bottomMLPIntermediateDimsOpt, numHiddenBottomMLPLayersOpt);
    topMLPIntermediateDims = getIntermediateDims(topMLPIntermediateDimsOpt,
                                                 numHiddenTopMLPLayersOpt);

    if (!lengthsMinMaxOpt.empty()) {
      assert(lengthsMinMaxOpt.size() == 2 &&
             "If min and max are used, must be 2 values provided");
      lengthsMin = lengthsMinMaxOpt[0];
      lengthsMax = lengthsMinMaxOpt[1];
      assert(lengthsMinMaxOpt[0] < lengthsMinMaxOpt[1] && "Min must be < max");
    }

    // Create TraceContext if trace file path is provided.
    if (!traceDir.empty()) {
      context_.setTraceContext(
          llvm::make_unique<TraceContext>(TraceEvent::TraceLevel::STANDARD));
    }

    // If device memory capacity is unset via command line, use 8MB by default.
    deviceMemCapacity =
        (int64_t)1024 *
        ((deviceMemCapacityOpt != 0) ? deviceMemCapacityOpt : 1024 * 8);

    numDevices = numDevicesOpt;
  }

  void TearDown() override {
    resultTensor = nullptr;
    bindings_->clear();

    auto *traceContext = context_.getTraceContext();

    if (traceContext) {
      // If traceContext exists, that means trace data was collected and needs
      // to be dumped to a file.

      // Get the test case and test names. They will be used to name the file.
      const ::testing::TestInfo *const testInfo =
          ::testing::UnitTest::GetInstance()->current_test_info();
      std::string testName(testInfo->name());
      std::string testCaseName(testInfo->test_case_name());

      // Replace all '/' in the test case and test names with '-' to preclude
      // errors related to directories not existing.
      for (auto &c : testName) {
        if (c == '/') {
          c = '-';
        }
      }

      for (auto &c : testCaseName) {
        if (c == '/') {
          c = '-';
        }
      }

      auto traceFileName =
          strFormat("%s/%s-%s.json", traceDir.getValue().c_str(),
                    testName.c_str(), testCaseName.c_str());
      traceContext->dump(traceFileName);
    }
  }

  /// Creates a Multi-layer perceptron network consisting of start & end FCs
  /// with \p intermediateLayers hidden layers.
  ///   * All weights and biases are random.
  ///   * All internal activations are RELU.
  ///   * Parent node \p N_ has output dimension \p inputDim.
  ///   * Hidden layers have dimension of \p intDim * intDim.
  ///   * Output layer has output dimension \p outputDim.
  static NodeValue createMLP(Module &mod, Function *F_, Node *N_,
                             size_t inputDim, llvm::ArrayRef<size_t> intDims,
                             size_t outputDim, size_t intermediateLayers) {
    assert(intermediateLayers > 0);

    const size_t firstIntDim = intDims[0];

    // Type object for the internal layers.
    // Note: dimension argument is a placeholder and will get filled out by each
    // createRandomizedConstant invocation.
    auto internalType = mod.uniqueType(ElemKind::FloatTy, {1});

    /// Initial
    auto *initial_bias = createRandomizedConstant(
        mod, internalType, {firstIntDim}, "initial_bias");
    auto *initial_weight = createRandomizedConstant(
        mod, internalType, {inputDim, firstIntDim}, "initial_weight");

    FullyConnectedNode *initial_layer = F_->createFullyConnected(
        "dense", N_, initial_weight,
        initial_bias); // Output is size {MB, intermediate dim}
    NodeValue last = F_->createRELU("relu1", initial_layer);

    /// Intermediate
    for (unsigned i = 0; i < intermediateLayers; ++i) {
      // The current intermediate dimension is based on the previous FC's
      // result's trailing dimension. Thus we set the current FC's trailing
      // weight dim equal to the next FC's intermediate dimension.
      const size_t intDim = intDims[i + 1];
      auto *intermediate_bias = createRandomizedConstant(
          mod, internalType, {intDim}, "intermediate_bias");
      auto *intermediate_weight = createRandomizedConstant(
          mod, internalType, {last.dims()[1], intDim}, "intermediate_weight");

      FullyConnectedNode *intermediate_layer = F_->createFullyConnected(
          "dense", last, intermediate_weight,
          intermediate_bias); // Output is size {MB, intDims[i]}
      last = F_->createRELU("relu2", intermediate_layer);
    }

    /// End
    auto *end_bias =
        createRandomizedConstant(mod, internalType, {outputDim}, "end_bias");
    auto *end_weight = createRandomizedConstant(
        mod, internalType, {last.dims()[1], outputDim}, "end_weight");

    FullyConnectedNode *end_layer = F_->createFullyConnected(
        "dense", last, end_weight, end_bias); // Output is size {MB, embDim}

    auto *RN = F_->createRELU("relu3", end_layer);

    return RN->getResult();
  }

  /// Creates a rowwise quantized Multi-layer perceptron network consisting of
  /// start & end FCs with \p intermediateLayers hidden layers.
  ///   * All weights and biases are random. Weights are Int8Q (rowwise), biases
  ///     are Int32.
  ///   * All internal activations are RELU, however the final layer has no
  ///     activation attached.
  ///   * Parent node \p N_ has output dimension \p inputDim int float.
  ///   * Hidden layers have dimension of \p intDim * intDim int Int8Q
  ///     (rowwise).
  ///   * Output layer has output dimension \p outputDim in float.
  ///
  /// Quantized MLPs use RowwiseQuantizedFullyConnected Nodes, which expect:
  ///   * weights to be Float32 and convert to Int8 fused rowwise quantized
  ///     Tensors internally
  ///   * Biases are Int32 quantized.
  static NodeValue createQuantizedMLP(Module &mod, Function *F_, NodeValue N_,
                                      size_t inputDim,
                                      llvm::ArrayRef<size_t> intDims,
                                      size_t outputDim,
                                      size_t intermediateLayers) {
    // Must have intermediate layers.
    assert(intermediateLayers > 0);

    const size_t firstIntDim = intDims[0];

    const size_t minibatchSize = N_.dims()[0];

    // Type objects for the internal types.
    // Note: dimension argument is a placeholder and will get filled out by each
    // createRandomizedConstant invocation.
    auto internalTypeF = mod.uniqueType(ElemKind::FloatTy, {1});
    auto internalTypeQ = mod.uniqueType(ElemKind::Int8QTy, {1}, 1, 0);
    auto internalBiasType = mod.uniqueType(ElemKind::Int32QTy, {1}, 1e-11, 0);

    auto *start = F_->createQuantize(
        "mlp_quant", N_, mod.uniqueTypeWithNewShape(internalTypeQ, N_.dims()));

    /// Initial.
    auto *initial_bias = createRandomizedConstant(
        mod, internalBiasType, {firstIntDim}, "initial_bias");
    auto *initial_weight = createRandomizedConstant(
        mod, internalTypeF, {inputDim, firstIntDim}, "initial_weight");

    // Output is size {MB, intermediatDim}
    quantization::Schema rowwiseQuantSchema = useSymmetricRowwiseQuantFCOpt
                                                  ? quantization::Symmetric
                                                  : quantization::Asymmetric;
    Node *initial_layer = F_->createRowwiseQuantizedFullyConnected(
        "dense", start, initial_weight, initial_bias,
        mod.uniqueTypeWithNewShape(internalTypeQ, {minibatchSize, firstIntDim}),
        rowwiseQuantSchema,
        /* transposeWeight */ true);

    NodeValue last = F_->createRELU("initial_relu", initial_layer);

    /// Intermediate
    for (unsigned i = 0; i < intermediateLayers; ++i) {
      // The current intermediate dimension is based on the previous FC's
      // result's trailing dimension. Thus we set the current FC's trailing
      // weight dim equal to the next FC's intermediate dimension.
      const size_t intDim = intDims[i + 1];
      auto *intermediate_bias = createRandomizedConstant(
          mod, internalBiasType, {intDim}, "intermediate_bias");
      auto *intermediate_weight = createRandomizedConstant(
          mod, internalTypeF, {last.dims()[1], intDim}, "intermediate_weight");

      Node *intermediate_layer = F_->createRowwiseQuantizedFullyConnected(
          "dense", last, intermediate_weight, intermediate_bias,
          mod.uniqueType(ElemKind::Int8QTy, {minibatchSize, intDim}, 1.0, 0),
          rowwiseQuantSchema,
          /* transposeWeight */ true); // Output is size {MB, intDims[i]}
      last = F_->createRELU("intermediate_relu", intermediate_layer);
    }

    /// End
    auto *end_bias = createRandomizedConstant(mod, internalBiasType,
                                              {outputDim}, "end_bias");
    auto *end_weight = createRandomizedConstant(
        mod, internalTypeF, {last.dims()[1], outputDim}, "end_weight");

    // Output is size {MB, embDim}
    auto *end_layer = F_->createRowwiseQuantizedFullyConnected(
        "dense", last, end_weight, end_bias,
        mod.uniqueTypeWithNewShape(internalTypeQ, {minibatchSize, outputDim}),
        rowwiseQuantSchema,
        /* transposeWeight */ true);

    auto *RN = F_->createRELU("relu", end_layer);
    auto *DQN = F_->createDequantize("mlp_dequant", RN);

    return DQN->getResult();
  }

  /// Creates a number of Sparse tables (FP32 or Int8Q), the Indices lookup and
  /// the SpareLengthsSum Node tying it together.
  void createSparseEmbeddings(Module &mod, PlaceholderBindings &bindings_,
                              Function *F_,
                              llvm::ArrayRef<Placeholder *> lengths,
                              llvm::ArrayRef<size_t> embSizes, size_t embDim,
                              std::vector<NodeValue> &embeddings) {
    auto internalTypeF = mod.uniqueType(ElemKind::FloatTy, {1});

    const ElemKind precisionForSLWS =
        useFP16SLWS ? ElemKind::Float16Ty : ElemKind::FloatTy;

    for (unsigned int i = 0; i < lengths.size(); i++) {
      fillStableRandomIndex(
          bindings_.allocate(lengths[i])->getHandle<int32_t>(), 2011,
          lengthsMin, lengthsMax);

      size_t sum =
          sumOfElements(bindings_.get(lengths[i])->getHandle<int32_t>());
      auto *indices = mod.createPlaceholder(
          ElemKind::Int64ITy, {sum}, "indices" + std::to_string(i), false);
      fillStableRandomIndex(bindings_.allocate(indices)->getHandle<int64_t>(),
                            2001, 0, embSizes[i]);

      // output is size {MB, embDim}
      if (quantizeSLWSData) {
        Constant *data = createRandomFusedRowwiseQuantizedConstant(
            mod, {embSizes[i], embDim}, "data" + std::to_string(i),
            useFP16SLWS);
        embeddings[i] = F_->createFusedRowwiseQuantizedSparseLengthsSum(
            "RQSLWS" + std::to_string(i), data, indices, lengths[i],
            precisionForSLWS, useFP16AccumSLWS);
        // Convert back to Float if we used Float16 here. Optimizer will
        // eliminate if necessary.
        if (useFP16SLWS) {
          embeddings[i] = F_->createConvertTo(
              "convert_" + embeddings[i].getNode()->getName().str(),
              embeddings[i], ElemKind::FloatTy);
        }
        F_->createSave("save_embedding", embeddings[i]);
      } else {
        Constant *data =
            createRandomizedConstant(mod, internalTypeF, {embSizes[i], embDim},
                                     "data" + std::to_string(i));
        embeddings[i] = F_->createSparseLengthsSum("sls" + std::to_string(i),
                                                   data, indices, lengths[i]);
      }
    }
  }

  /// Creates a number of Sparse tables (FP32 or Int8Q), the Indices lookup and
  /// the SpareLengthsSum Node tying it together.
  void createSparseWeightedGatherEmbeddings(
      Module &mod, PlaceholderBindings &bindings_, Function *F_,
      llvm::ArrayRef<Placeholder *> lengths, llvm::ArrayRef<size_t> tableSizes,
      size_t embeddingDim, std::vector<NodeValue> &embeddings,
      uint32_t weightsSize = 1000) {
    const ElemKind precisionForSLWS =
        useFP16SLWS ? ElemKind::Float16Ty : ElemKind::FloatTy;

    for (size_t i = 0; i < lengths.size(); i++) {
      fillStableRandomIndex(
          bindings_.allocate(lengths[i])->getHandle<int32_t>(), 2011,
          lengthsMin, lengthsMax);

      size_t sum =
          sumOfElements(bindings_.get(lengths[i])->getHandle<int32_t>());
      auto *indices = mod.createPlaceholder(
          ElemKind::Int64ITy, {sum}, "indices" + std::to_string(i), false);
      fillStableRandomIndex(bindings_.allocate(indices)->getHandle<int64_t>(),
                            2001, 0, tableSizes[i]);

      // Should be able to pass weights - fix later. Currently, just a
      // randomized constant.
      Constant *weightsConst = createRandomizedConstant(
          mod, mod.uniqueType(ElemKind::FloatTy, {weightsSize}), {weightsSize},
          "weights" + std::to_string(i));

      auto *weightIndices =
          mod.createPlaceholder(ElemKind::Int32ITy, {sum},
                                "weight_indices" + std::to_string(i), false);
      fillStableRandomIndex(
          bindings_.allocate(weightIndices)->getHandle<int32_t>(), 2001, 0,
          weightsSize - 1);

      auto *weights = F_->createGather("weight_gather" + std::to_string(i),
                                       weightsConst, weightIndices, 0);

      // output is size {MB, embeddingDim_}
      if (quantizeSLWSData) {
        Constant *data = createRandomFusedRowwiseQuantizedConstant(
            mod, {tableSizes[i], embeddingDim}, "data" + std::to_string(i),
            useFP16SLWS);
        embeddings[i] = F_->createFusedRowwiseQuantizedSparseLengthsWeightedSum(
            "RQSLWS" + std::to_string(i), data, weights, indices, lengths[i],
            precisionForSLWS, useFP16AccumSLWS);
        // Convert back to Float if we used Float16 here. Optimizer will
        // eliminate if necessary.
        if (useFP16SLWS) {
          embeddings[i] = F_->createConvertTo(
              "convert_" + embeddings[i].getNode()->getName().str(),
              embeddings[i], ElemKind::FloatTy);
        }
      } else {
        Constant *data = createRandomizedConstant(
            mod,
            mod.uniqueType(ElemKind::FloatTy, {tableSizes[i], embeddingDim}),
            {tableSizes[i], embeddingDim}, "data" + std::to_string(i));
        embeddings[i] = F_->createSparseLengthsWeightedSum(
            "slws" + std::to_string(i), data, weights, indices, lengths[i]);
      }
    }
  }

  /// Builds a simple graph, \returns the Tensor output of the graph.
  Tensor *createSimpleRecSysGraph(Module &mod, PlaceholderBindings &bindings,
                                  Function *F, llvm::ArrayRef<size_t> embSizes,
                                  size_t embDim) {
    EXPECT_EQ(tableSizes.size(), embSizes.size());

    // Create the tables.
    std::vector<Placeholder *> lengths(tableSizes.size());
    for (unsigned int i = 0; i < lengths.size(); i++) {
      lengths[i] = mod.createPlaceholder(ElemKind::Int32ITy, {miniBatch},
                                         "SL" + std::to_string(i), false);
    }

    auto *denseData = mod.createPlaceholder(ElemKind::FloatTy,
                                            {miniBatch, denseDim}, "denseData",
                                            false); // denseDim can be anything

    // First Dense embedding
    fillStableRandomData(bindings.allocate(denseData)->getHandle(), 2001,
                         0.001);
    NodeValue bottomMLP;
    if (quantizeFC) {
      bottomMLP = createQuantizedMLP(mod, F, denseData, denseData->dims()[1],
                                     bottomMLPIntermediateDims, embDim,
                                     numHiddenBottomMLPLayersOpt);
    } else {
      bottomMLP = createMLP(mod, F, denseData, denseData->dims()[1],
                            bottomMLPIntermediateDims, embDim,
                            numHiddenBottomMLPLayersOpt);
    }

    // Sparse Embeddings
    std::vector<NodeValue> embeddings(lengths.size());
    if (gatherWeights) {
      createSparseWeightedGatherEmbeddings(mod, bindings, F, lengths, embSizes,
                                           embDim, embeddings);
    } else {
      createSparseEmbeddings(mod, bindings, F, lengths, embSizes, embDim,
                             embeddings);
    }

    // Interacting sparse and dense
    embeddings.push_back(bottomMLP);
    std::cout << "Number of embeddings concatenated: " << embeddings.size()
              << std::endl;
    auto *CN = F->createConcat("concat", embeddings,
                               1); // Output is size {MB, embDim*n}
    auto *reshaped = F->createReshape(
        "reshape", CN,
        {bottomMLP.dims()[0], embeddings.size(), embDim}); // {MB, n, embDim}
    auto *transposed =
        F->createTranspose("transpose", reshaped, {0, 2, 1}); // {MB, embDim, n}
    auto *dot = F->createBatchMatMul("dot_products", reshaped,
                                     transposed); // {MB, n, n}
    auto *reshapeDot =
        F->createReshape("reshapeDot", dot,
                         {bottomMLP.dims()[0],
                          embeddings.size() * embeddings.size()}); // {MB, n^2}
    NodeValue interact = F->createConcat("interact", {reshapeDot, bottomMLP},
                                         1); // {MB, n^2 + embDim}

    // MLP at the top
    Node *topMLP;
    if (quantizeFC) {
      topMLP = createQuantizedMLP(mod, F, interact, interact.dims()[1],
                                  topMLPIntermediateDims,
                                  /* outputDim */ 1, numHiddenTopMLPLayersOpt);
    } else {
      topMLP = createMLP(mod, F, interact, interact.dims()[1],
                         topMLPIntermediateDims,
                         /* outputDim */ 1, numHiddenTopMLPLayersOpt);
    }

    // Output
    auto *save = F->createSave("save", topMLP);

    return bindings.allocate(save->getPlaceholder());
  }

  /// Set up the precision configuration. This will be used for all
  /// compilations which are compared to (Interpreter/Partitioned).
  void setupPrecisionConfig() {
    if (convertToFP16) {
      precConfig_.convertToFP16 = convertToFP16;
      // For now always convert both or neither.
      precConfig_.convertFusedToFP16 = convertToFP16;
      // Note: always do not convert RWQ-SLWS here. The creator itself for
      // precisionForNonDataSLWS already directly created the node with the
      // correct precision.
      precConfig_.precisionModeKindSet.insert(
          Kinded::Kind::FusedRowwiseQuantizedSparseLengthsWeightedSumNodeKind);
      precConfig_.precisionModeKindSet.insert(
          Kinded::Kind::RowwiseQuantizedFullyConnectedNodeKind);
    }
  }

  void testRecSys(bool checkConcat = false) {
    assert((!useFP16AccumSLWS || useFP16SLWS) &&
           "Can only use FP16 accumulation when using FP16 precision.");

    setupPrecisionConfig();

    // Generate the network.
    std::unique_ptr<Module> mod(new Module);
    F_ = mod->createFunction("main");
    resultTensor = createSimpleRecSysGraph(*mod.get(), *bindings_, F_,
                                           tableSizes, embeddingDim);

    Placeholder *concatPH = nullptr;
    if (checkConcat) {
      // Add an observer node after concat.
      auto *CN = F_->getNodeByName("concat");
      auto *saveConcat = F_->createSave("after_concat_data", CN);
      concatPH = saveConcat->getPlaceholder();
    }
    auto configs = generateDeviceConfigs(getBackendName(), 1, MAX_MEMORY);
    std::unique_ptr<HostManager> hostManager(
        new HostManager(std::move(configs)));

    CompilationContext cctx;
    cctx.precisionConfig = precConfig_;
    EXIT_ON_ERR(hostManager->addNetwork(std::move(mod), cctx));

    // Run graph
    ExecutionContext context2{};
    dispatchInference(hostManager.get(), context_);

    // NaNs are a sign of something gone wrong. Always verify there aren't any
    // in the result.
    auto resultTensorH = resultTensor->getHandle();
    for (size_t i = 0, e = resultTensorH.size(); i < e; i++) {
      EXPECT_FALSE(std::isnan(resultTensorH.raw(i)));
    }

    if (checkConcat) {
      // Get result and verify.
      EXPECT_EQ(resultTensor->size(), miniBatch);

      auto *concatT = bindings_->get(concatPH);
      auto concatH = concatT->getHandle();
      // Check that intermediate concat results didn't overflow.
      std::cout << "Intermediate concats" << std::endl;
      concatH.dump();
      for (int i = 0, e = concatH.size(); i < e; ++i) {
        EXPECT_LE(fabs(concatH.raw(i)), 100);
      }

      std::cout << "Result of prediction" << std::endl;
      std::cout << resultTensorH.size() << std::endl;
      resultTensorH.dump();
      for (int i = 0, e = resultTensorH.size(); i < e; ++i) {
        EXPECT_GE(resultTensorH.raw(i), 0.0);
      }
    }

    // Compare against interpreter if we're not executing already on it.
    if (getBackendName() != "Interpreter") {
      compareAgainstInterpreter();
    }
  }

  /// Run on the Interpreter and compare the result to previous result.
  void compareAgainstInterpreter() {
    ExecutionContext contextI;
    // Create a new module for the interpreter run.
    std::unique_ptr<Module> modI(new Module);
    auto *IF = modI->createFunction("main");
    PlaceholderBindings *bindingsI = contextI.getPlaceholderBindings();
    Tensor *resultIT = createSimpleRecSysGraph(*modI, *bindingsI, IF,
                                               tableSizes, embeddingDim);
    bindingsI->allocate(modI->getPlaceholders());

    // Set device memory to 64GB to prevent partitioning. We are using the
    // Interpreter's result just as a reference result to compare against.
    auto configs = generateDeviceConfigs("Interpreter", 1, MAX_MEMORY);
    std::unique_ptr<HostManager> hostManager(
        new HostManager(std::move(configs)));

    // Use the same precision transformation for compilation.
    CompilationContext cctx;
    cctx.precisionConfig = precConfig_;
    EXIT_ON_ERR(hostManager->addNetwork(std::move(modI), cctx));
    dispatchInference(hostManager.get(), contextI);

    assert(resultTensor && "Must run and set resultTensor before comparing "
                           "against the intepreter.");
    EXPECT_TRUE(resultIT->isEqual(*resultTensor));
  }

  /// Create partitions to run and compare results.
  void testPartitionedRecSys(size_t numDevices, size_t memSize,
                             ExecutionContext &context) {
    // Result tensors are reused below, so create a local copy.
    Tensor referenceResultT = resultTensor->clone();
    // Generate configs and create a new HostManager for testing partitioning.
    auto configs = generateDeviceConfigs(getBackendName(), numDevices, memSize);
    std::unique_ptr<HostManager> hostManager(
        new HostManager(std::move(configs)));

    // Create a new module and placeholderBindings to run on the partitioning
    // HostManager.
    PlaceholderBindings bindingsP;
    std::unique_ptr<Module> modP(new Module);
    // Since HostManager consumed the uniquePtr we grab a raw pointer to the
    // module so we can verify partitioning.
    Module *rawModule = modP.get();
    auto *funcP = modP->createFunction("main");
    createSimpleRecSysGraph(*modP, bindingsP, funcP, tableSizes, embeddingDim);

    assert(memSize > 0 && "Must set partitionerPerDeviceMemCapacity > 0.");
    assert(numDevices > 0 && "Must set partitionerNumDevices > 0.");
    std::cout << numDevices << " devices of size " << memSize << "\n";
    // Use the same precision transformation for compilation.
    CompilationContext cctx;
    cctx.precisionConfig = precConfig_;
    EXIT_ON_ERR(hostManager->addNetwork(std::move(modP), cctx));
    std::cout << "Partitions = " << rawModule->getFunctions().size()
              << std::endl;
    ASSERT_LE(rawModule->getFunctions().size(), numDevices);

    // Run the partitioned graph and compare the results.
    auto &bindings = *context.getPlaceholderBindings();
    bindings.clear();
    bindings.allocate(rawModule->getPlaceholders());
    bindingsP.allocate(rawModule->getPlaceholders());
    for (auto PH : bindingsP.pairs()) {
      bindingsP.copyToTarget(PH.first->getName(), bindings);
    }

    dispatchInference(hostManager.get(), context);

    Tensor *resultTensorP = bindings.get(bindings.getPlaceholderByName("save"));
    EXPECT_TRUE(referenceResultT.isEqual(*resultTensorP));
  }

  /// Test SparseLengthsSum independently.
  void testSLSQuant() {
    std::unique_ptr<Module> mod(new Module);
    F_ = mod->createFunction("main");
    std::vector<Placeholder *> sparseLengths(1);
    sparseLengths[0] =
        mod->createPlaceholder(ElemKind::Int32ITy, {miniBatch}, "SL0", false);

    std::vector<NodeValue> embeddings(sparseLengths.size());
    createSparseEmbeddings(*mod.get(), *bindings_, F_, sparseLengths,
                           tableSizes, embeddingDim, embeddings);

    auto *save = F_->createSave("save", embeddings[0]);
    Tensor *resultTensorLocal = bindings_->allocate(save->getPlaceholder());

    // Use the same precision transformation for compilation.
    CompilationContext cctx;
    cctx.precisionConfig = precConfig_;
    auto configs = generateDeviceConfigs(getBackendName(), 1, MAX_MEMORY);
    std::unique_ptr<HostManager> hostManager(
        new HostManager(std::move(configs)));
    EXIT_ON_ERR(hostManager->addNetwork(std::move(mod), cctx));

    // Run graph.
    dispatchInference(hostManager.get(), context_);

    // TODO: for now we only check the output dimension, contents are ignored
    EXPECT_EQ(resultTensorLocal->size(), miniBatch * embeddingDim);
    resultTensorLocal->getHandle().dump();
  }
};

/// Standard Tests
/// These tests have three options:
///   * quantizeSLWSData enables Int8 Fused Rowwise Quantization for the Sparse
///     Embeddings (Int8 quantized values with float scale and offset).
///   * quantizeFC enables Int8 Fused Rowwise Quantization for FC weights and
///     activations inside the MLPs.
///   * convertToFP16 walks the graph at the end of constructing the graph and
///     converts all FP32 nodes & tensors to FP16, meaning the graph will use
///     FP16 for internal weights, biases and activations (when not already Int8
///     quantized). Inputs and outputs are still FP32 but are immediately
///     dropped to FP16 precision at the beginning of the graph.
///   * useFP16SLWS represents whether to use Float16 for non-data
///     inputs/outputs for SLWS and SLS Nodes, and for data per-row scale and
///     offset.
///   * useFP16AccumSLWS represents whether to use Float16 accumulation for SLWS
///     and SLS Nodes. Note this should only be used if useFP16SLWS.

/// Everything in FP32.
TEST_P(RecommendationSystemTest, RecSys_FP32) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = false;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();
}

/// Rowwise quantize the SLWS and FC; everything else in FP32.
TEST_P(RecommendationSystemTest, RecSys_RWQuantized_SLWS_FC) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = true;
  convertToFP16 = false;

  testRecSys();
}

/// Rowwise quantize the SLWS; everything else in FP32.
TEST_P(RecommendationSystemTest, RecSys_RWQuantized_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();
}

/// Rowwise quantize the SLWS and FC; everything else in FP16.
TEST_P(RecommendationSystemTest, RecSys_RWQuantized_SLWS_FC_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = true;
  convertToFP16 = true;

  testRecSys();
}

/// Rowwise quantize the SLWS; everything else in FP16.
TEST_P(RecommendationSystemTest, RecSys_RWQuantized_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16. Everything else in FP32.
TEST_P(RecommendationSystemTest, RecSys_RWQuantizedFP16_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16, and use FP16 accumulation. Everything else in FP32.
TEST_P(RecommendationSystemTest, RecSys_RWQuantizedFP16AccumFP16_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = true;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16. Everything else in FP16.
TEST_P(RecommendationSystemTest, RecSys_RWQuantizedFP16_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16, and use FP16 accumulation. Everything else in FP16.
TEST_P(RecommendationSystemTest, RecSys_RWQuantizedFP16AccumFP16_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = true;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();
}

/// Partitioning Tests
/// These tests have the same options as the above, but also partition the
/// created graph into segments and walk the dag. The test then compares output
/// for the partitioned and unpartitioned runs.

TEST_P(RecommendationSystemTest, RecSys_FP32_Partitioned) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = false;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();

  // If the memory capacity was not set on the command line, then double the
  // default value for this test.
  if (deviceMemCapacityOpt == 0) {
    deviceMemCapacity *= 2; // Double memory for this test
  }

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantized_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();

  // If the memory capacity was not set on the command line, then double the
  // default value for this test.
  if (deviceMemCapacityOpt == 0) {
    deviceMemCapacity *= 2; // Double memory for this test
  }

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantized_SLWS_FC) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = true;
  convertToFP16 = false;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantized_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantized_SLWS_FC_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = true;
  convertToFP16 = true;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16. Everything else in FP32. Also run partitioned and
/// compare results.
TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantizedFP16_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();

  // If the memory capacity was not set on the command line, then double the
  // default value for this test.
  if (deviceMemCapacityOpt == 0) {
    deviceMemCapacity *= 2; // Double memory for this test
  }

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16, and use FP16 accumulation. Everything else in FP32.
/// Also run partitioned and compare results.
TEST_P(RecommendationSystemTest,
       RecSys_Partitioned_RWQuantizedFP16AccumFP16_SLWS) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = true;
  quantizeFC = false;
  convertToFP16 = false;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16. Everything else in FP16. Also run partitioned and
/// compare results.
TEST_P(RecommendationSystemTest, RecSys_Partitioned_RWQuantizedFP16_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

/// Rowwise quantize the SLWS, with FP16 for scales/bias, and other
/// inputs/outputs in FP16, and use FP16 accumulation. Everything else in FP16.
/// Also run partitioned and compare results.
TEST_P(RecommendationSystemTest,
       RecSys_Partitioned_RWQuantizedFP16AccumFP16_SLWS_FP16) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;
  useFP16SLWS = true;
  useFP16AccumSLWS = true;
  quantizeFC = false;
  convertToFP16 = true;

  testRecSys();

  testPartitionedRecSys(numDevices, deviceMemCapacity, context_);
}

/// Test SLS independently, with no other layers being run.
TEST_P(RecommendationSystemTest, RecSys_SLS_Only) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = true;

  // Normally called in testRecSys(), but we're bypassing it here.
  setupPrecisionConfig();

  testSLSQuant();
}

/// Test gathering weights for SLWS.
TEST_P(RecommendationSystemTest, RecSys_FP32_Gather_Weights) {
  CHECK_IF_ENABLED();

  quantizeSLWSData = false;
  useFP16SLWS = false;
  useFP16AccumSLWS = false;
  quantizeFC = false;
  convertToFP16 = false;

  gatherWeights = true;

  testRecSys();
}

INSTANTIATE_BACKEND_TEST(RecommendationSystemTest);
