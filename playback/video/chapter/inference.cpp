#include "playback/video/chapter/inference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ggml-backend.h>
#include <llama.h>
#include <mtmd.h>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

constexpr std::uint32_t kContextTokens = 8192;
constexpr std::int32_t kBatchTokens = 2048;
constexpr std::int32_t kMicroBatchTokens = 512;
constexpr int kMaximumGeneratedTokens = 2048;
constexpr std::size_t kMaximumJsonBytes = 128u * 1024u;

// Verbatim grammar from llama.cpp b7146 grammars/json.gbnf. The generated
// response is constrained at sampling time; semantic validation remains in the
// Radioify domain parser.
constexpr std::string_view kJsonGrammar = R"gbnf(
root   ::= object
value  ::= object | array | string | number | ("true" | "false" | "null") ws

object ::=
  "{" ws (
            string ":" ws value
    ("," ws string ":" ws value)*
  )? "}" ws

array  ::=
  "[" ws (
            value
    ("," ws value)*
  )? "]" ws

string ::=
  "\"" (
    [^"\\\x7F\x00-\x1F] |
    "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4})
  )* "\"" ws

number ::= ("-"? ([0-9] | [1-9] [0-9]{0,15})) ("." [0-9]+)? ([eE] [-+]? [0-9] [1-9]{0,15})? ws

ws ::= | " " | "\n" [ \t]{0,20}
)gbnf";

bool cancelled(const OperationControl& control) {
  try {
    return control.cancelled && control.cancelled();
  } catch (...) {
    return true;
  }
}

bool gpuRevoked(const OperationControl& control) {
  try {
    return control.backgroundGpuAllowed &&
           !control.backgroundGpuAllowed();
  } catch (...) {
    return true;
  }
}

OperationStatus interruptionStatus(const OperationControl& control) {
  if (cancelled(control)) return OperationStatus::Cancelled;
  if (gpuRevoked(control)) return OperationStatus::Yielded;
  return OperationStatus::Failed;
}

InferenceResult interruptedResult(const OperationControl& control) {
  const OperationStatus status = interruptionStatus(control);
  if (status == OperationStatus::Cancelled) {
    return {status, "Chapter analysis cancelled.", {}};
  }
  if (status == OperationStatus::Yielded) {
    return {status, "Playback reclaimed the GPU.", {}};
  }
  return {};
}

bool continueOperation(const OperationControl& control) {
  return !cancelled(control) && !gpuRevoked(control);
}

bool backendNameEquals(ggml_backend_dev_t device,
                       std::string_view expected) {
  if (!device) return false;
  const ggml_backend_reg_t registry =
      ggml_backend_dev_backend_reg(device);
  const char* name = registry ? ggml_backend_reg_name(registry) : nullptr;
  if (!name || std::strlen(name) != expected.size()) return false;
  return std::equal(expected.begin(), expected.end(), name,
                    [](unsigned char left, unsigned char right) {
                      if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
                      if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
                      return left == right;
                    });
}

ggml_backend_dev_t defaultVulkanDevice() {
  // libmtmd b7146 selects ggml's default GPU. Requiring that exact
  // typed device to be Vulkan keeps the text model and projector on the same
  // backend without process environment overrides or log inspection.
  ggml_backend_dev_t device =
      ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
  if (!device || !backendNameEquals(device, "Vulkan")) return nullptr;
  const ggml_backend_buffer_type_t bufferType =
      ggml_backend_dev_buffer_type(device);
  if (!bufferType || ggml_backend_buft_is_host(bufferType)) return nullptr;
  return device;
}

int inferenceThreads() {
  const unsigned detected = std::thread::hardware_concurrency();
  return static_cast<int>(std::max(1u, std::min(4u, detected ? detected : 1u)));
}

struct LoadProgress {
  const OperationControl* control = nullptr;
  double begin = 0.0;
  double span = 0.0;
  const char* phase = nullptr;
  std::atomic<bool> callbackFailed{false};
};

bool modelLoadProgress(float progress, void* opaque) {
  auto* state = static_cast<LoadProgress*>(opaque);
  if (!state || !state->control || !continueOperation(*state->control)) {
    return false;
  }
  try {
    if (state->control->progress) {
      state->control->progress(
          state->begin + state->span * std::clamp<double>(progress, 0.0, 1.0),
          state->phase ? state->phase : "Loading chapter model");
    }
  } catch (...) {
    state->callbackFailed.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool abortInference(void* opaque) {
  const auto* control = static_cast<const OperationControl*>(opaque);
  return !control || !continueOperation(*control);
}

void discardLog(ggml_log_level, const char*, void*) {}

// llama.cpp's backend registry is process-wide. A function-local owner gives it
// one balanced lifetime even if more than one chapter service exists, while
// lazy construction keeps backend discovery off the application/UI thread.
class ProcessRuntime final {
 public:
  ProcessRuntime() {
    llama_log_set(&discardLog, nullptr);
    mtmd_log_set(&discardLog, nullptr);
    llama_backend_init();
  }

  ~ProcessRuntime() { llama_backend_free(); }

  ProcessRuntime(const ProcessRuntime&) = delete;
  ProcessRuntime& operator=(const ProcessRuntime&) = delete;
};

ProcessRuntime& processRuntime() {
  static ProcessRuntime runtime;
  return runtime;
}

using ModelPtr = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using ContextPtr = std::unique_ptr<llama_context, decltype(&llama_free)>;
using VisionPtr = std::unique_ptr<mtmd_context, decltype(&mtmd_free)>;
using BitmapPtr = std::unique_ptr<mtmd_bitmap, decltype(&mtmd_bitmap_free)>;
using ChunksPtr =
    std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)>;
using SamplerPtr =
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)>;

class Batch final {
 public:
  explicit Batch(int32_t capacity)
      : value_(llama_batch_init(capacity, 0, 1)) {}
  ~Batch() { llama_batch_free(value_); }

  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;

  llama_batch* operator->() { return &value_; }
  llama_batch& get() { return value_; }

 private:
  llama_batch value_{};
};

std::string pathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  return wideToUtf8Lossy(path.wstring());
#else
  return path.string();
#endif
}

std::optional<std::string> formatPrompt(const llama_model* model,
                                        std::string content) {
  if (!model) return std::nullopt;
  const char* marker = mtmd_default_marker();
  if (!marker || !*marker) return std::nullopt;
  if (content.find(marker) == std::string::npos) content += marker;

  const char* chatTemplate = llama_model_chat_template(model, nullptr);
  if (!chatTemplate || !*chatTemplate) return std::nullopt;
  const llama_chat_message message{"user", content.c_str()};
  std::vector<char> formatted(std::max<std::size_t>(1024, content.size() * 2));
  int32_t written = llama_chat_apply_template(
      chatTemplate, &message, 1, true, formatted.data(),
      static_cast<int32_t>(std::min<std::size_t>(
          formatted.size(), static_cast<std::size_t>(INT32_MAX))));
  if (written < 0) return std::nullopt;
  if (static_cast<std::size_t>(written) >= formatted.size()) {
    formatted.resize(static_cast<std::size_t>(written) + 1u);
    written = llama_chat_apply_template(
        chatTemplate, &message, 1, true, formatted.data(),
        static_cast<int32_t>(std::min<std::size_t>(
            formatted.size(), static_cast<std::size_t>(INT32_MAX))));
  }
  if (written < 0 || static_cast<std::size_t>(written) >= formatted.size()) {
    return std::nullopt;
  }
  return std::string(formatted.data(), static_cast<std::size_t>(written));
}

std::optional<std::string> tokenPiece(const llama_vocab* vocab,
                                      llama_token token) {
  if (!vocab) return std::nullopt;
  std::array<char, 256> local{};
  int32_t length = llama_token_to_piece(vocab, token, local.data(),
                                        static_cast<int32_t>(local.size()), 0,
                                        false);
  if (length >= 0) {
    return std::string(local.data(), static_cast<std::size_t>(length));
  }
  if (length == (std::numeric_limits<int32_t>::min)()) return std::nullopt;
  const std::size_t required = static_cast<std::size_t>(-length);
  if (required > kMaximumJsonBytes) return std::nullopt;
  std::vector<char> expanded(required);
  length = llama_token_to_piece(vocab, token, expanded.data(),
                                static_cast<int32_t>(expanded.size()), 0,
                                false);
  if (length < 0) return std::nullopt;
  return std::string(expanded.data(), static_cast<std::size_t>(length));
}

std::size_t totalChunkTokens(const mtmd_input_chunks* chunks) {
  std::size_t total = 0;
  if (!chunks) return total;
  for (std::size_t index = 0; index < mtmd_input_chunks_size(chunks);
       ++index) {
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, index);
    const std::size_t count = chunk ? mtmd_input_chunk_get_n_tokens(chunk) : 0;
    if (count > (std::numeric_limits<std::size_t>::max)() - total) return 0;
    total += count;
  }
  return total;
}

std::size_t totalChunkPositions(const mtmd_input_chunks* chunks) {
  std::size_t total = 0;
  if (!chunks) return total;
  for (std::size_t index = 0; index < mtmd_input_chunks_size(chunks);
       ++index) {
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, index);
    const llama_pos count = chunk ? mtmd_input_chunk_get_n_pos(chunk) : 0;
    if (count <= 0 || static_cast<std::size_t>(count) >
                          (std::numeric_limits<std::size_t>::max)() - total) {
      return 0;
    }
    total += static_cast<std::size_t>(count);
  }
  return total;
}

int32_t evaluateTextChunk(llama_context* context,
                          const mtmd_input_chunk* chunk, llama_pos* nPast,
                          bool logitsLast, int32_t nBatch,
                          const OperationControl& control) {
  if (!context || !chunk || !nPast || nBatch <= 0) return -1;
  std::size_t tokenCount = 0;
  const llama_token* tokens =
      mtmd_input_chunk_get_tokens_text(chunk, &tokenCount);
  if (!tokens || tokenCount == 0) return -1;
  Batch batch(nBatch);
  if (!batch->token || !batch->pos || !batch->n_seq_id ||
      !batch->seq_id || !batch->logits) {
    return -1;
  }
  std::size_t offset = 0;
  while (offset < tokenCount) {
    if (!continueOperation(control)) return 2;
    batch->n_tokens = 0;
    while (offset < tokenCount && batch->n_tokens < nBatch) {
      const int32_t index = batch->n_tokens++;
      batch->token[index] = tokens[offset++];
      batch->pos[index] = (*nPast)++;
      batch->n_seq_id[index] = 1;
      batch->seq_id[index][0] = 0;
      batch->logits[index] = 0;
    }
    if (logitsLast && offset == tokenCount) {
      batch->logits[batch->n_tokens - 1] = 1;
    }
    const int32_t decoded = llama_decode(context, batch.get());
    if (decoded != 0) return decoded;
  }
  return 0;
}

int32_t evaluateImageChunk(mtmd_context* vision, llama_context* context,
                           const mtmd_input_chunk* chunk, llama_pos* nPast,
                           int32_t nBatch,
                           const OperationControl& control) {
  if (!vision || !context || !chunk || !nPast || nBatch <= 0 ||
      mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_IMAGE) {
    return -1;
  }
  if (!continueOperation(control)) return 2;
  if (mtmd_encode_chunk(vision, chunk) != 0) return 1;
  if (!continueOperation(control)) return 2;

  float* embeddings = mtmd_get_output_embd(vision);
  const std::size_t tokenCount = mtmd_input_chunk_get_n_tokens(chunk);
  const int32_t embeddingSize =
      llama_model_n_embd_inp(llama_get_model(context));
  if (!embeddings || tokenCount == 0 ||
      tokenCount > static_cast<std::size_t>(INT32_MAX) || embeddingSize <= 0) {
    return -1;
  }

  const bool mrope = mtmd_decode_use_mrope(vision);
  const std::size_t positionDimensions = mrope ? 4u : 1u;
  if (tokenCount > (std::numeric_limits<std::size_t>::max)() /
                       positionDimensions) {
    return -1;
  }
  std::vector<llama_pos> positions(tokenCount * positionDimensions);
  std::vector<int32_t> sequenceCounts(tokenCount, 1);
  std::vector<llama_seq_id> sequenceZero(1, 0);
  std::vector<llama_seq_id*> sequences(tokenCount, sequenceZero.data());
  std::vector<int8_t> logits(tokenCount, 0);

  if (mrope) {
    const mtmd_image_tokens* imageTokens =
        mtmd_input_chunk_get_tokens_image(chunk);
    if (!imageTokens) return -1;
    const std::size_t width = mtmd_image_tokens_get_nx(imageTokens);
    const std::size_t height = mtmd_image_tokens_get_ny(imageTokens);
    if (width == 0 || height == 0 || width > tokenCount / height ||
        width * height != tokenCount) {
      return -1;
    }
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        const std::size_t index = y * width + x;
        positions[index] = *nPast;
        positions[index + tokenCount] =
            *nPast + static_cast<llama_pos>(y);
        positions[index + tokenCount * 2u] =
            *nPast + static_cast<llama_pos>(x);
        positions[index + tokenCount * 3u] = 0;
      }
    }
  } else {
    for (std::size_t index = 0; index < tokenCount; ++index) {
      positions[index] = *nPast + static_cast<llama_pos>(index);
    }
  }

  const bool nonCausal = mtmd_decode_use_non_causal(vision);
  if (nonCausal) llama_set_causal_attn(context, false);
  struct CausalRestore {
    llama_context* context = nullptr;
    bool restore = false;
    ~CausalRestore() {
      if (context && restore) llama_set_causal_attn(context, true);
    }
  } causalRestore{context, nonCausal};

  for (std::size_t offset = 0; offset < tokenCount;) {
    if (!continueOperation(control)) return 2;
    const int32_t count = static_cast<int32_t>(std::min<std::size_t>(
        static_cast<std::size_t>(nBatch), tokenCount - offset));
    std::vector<llama_pos> positionView;
    llama_pos* batchPositions = positions.data() + offset;
    if (mrope) {
      positionView.reserve(static_cast<std::size_t>(count) * 4u);
      for (std::size_t dimension = 0; dimension < 4u; ++dimension) {
        const llama_pos* begin =
            positions.data() + dimension * tokenCount + offset;
        positionView.insert(positionView.end(), begin, begin + count);
      }
      batchPositions = positionView.data();
    }
    llama_batch batch{};
    batch.n_tokens = count;
    batch.embd = embeddings + offset * static_cast<std::size_t>(embeddingSize);
    batch.pos = batchPositions;
    batch.n_seq_id = sequenceCounts.data() + offset;
    batch.seq_id = sequences.data() + offset;
    batch.logits = logits.data() + offset;
    const int32_t decoded = llama_decode(context, batch);
    if (decoded != 0) return decoded;
    offset += static_cast<std::size_t>(count);
  }
  *nPast += mtmd_input_chunk_get_n_pos(chunk);
  return 0;
}

int32_t evaluateChunks(mtmd_context* vision, llama_context* context,
                       const mtmd_input_chunks* chunks, llama_pos* nPast,
                       int32_t nBatch, const OperationControl& control) {
  if (!vision || !context || !chunks || !nPast) return -1;
  const std::size_t count = mtmd_input_chunks_size(chunks);
  if (count == 0) return -1;
  for (std::size_t index = 0; index < count; ++index) {
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, index);
    if (!chunk) return -1;
    const mtmd_input_chunk_type type = mtmd_input_chunk_get_type(chunk);
    int32_t evaluated = -1;
    if (type == MTMD_INPUT_CHUNK_TYPE_TEXT) {
      evaluated = evaluateTextChunk(context, chunk, nPast,
                                    index + 1u == count, nBatch, control);
    } else if (type == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
      evaluated = evaluateImageChunk(vision, context, chunk, nPast, nBatch,
                                     control);
    }
    if (evaluated != 0) return evaluated;
  }
  return 0;
}

SamplerPtr createJsonSampler(const llama_vocab* vocab) {
  SamplerPtr chain(llama_sampler_chain_init(
                       llama_sampler_chain_default_params()),
                   &llama_sampler_free);
  if (!chain) return {nullptr, &llama_sampler_free};
  llama_sampler* grammar = llama_sampler_init_grammar(
      vocab, kJsonGrammar.data(), "root");
  if (!grammar) return {nullptr, &llama_sampler_free};
  llama_sampler_chain_add(chain.get(), grammar);
  const auto add = [&](llama_sampler* sampler) {
    if (!sampler) return false;
    llama_sampler_chain_add(chain.get(), sampler);
    return true;
  };
  if (!add(llama_sampler_init_top_k(40)) ||
      !add(llama_sampler_init_top_p(0.95f, 1)) ||
      !add(llama_sampler_init_min_p(0.05f, 1)) ||
      !add(llama_sampler_init_temp(0.2f)) ||
      !add(llama_sampler_init_dist(0))) {
    return {nullptr, &llama_sampler_free};
  }
  return chain;
}

InferenceResult generateJson(llama_context* context, const llama_vocab* vocab,
                             llama_pos nPast, int maximumGeneratedTokens,
                             const OperationControl& control) {
  if (!context || !vocab || maximumGeneratedTokens <= 0) {
    return {OperationStatus::Failed,
            "The inference context has no room for chapter output.", {}};
  }
  SamplerPtr sampler = createJsonSampler(vocab);
  if (!sampler) {
    return {OperationStatus::Failed,
            "Could not initialize constrained JSON generation.", {}};
  }
  std::string output;
  output.reserve(16u * 1024u);
  for (int generated = 0; generated < maximumGeneratedTokens; ++generated) {
    if (!continueOperation(control)) return interruptedResult(control);
    const llama_token token =
        llama_sampler_sample(sampler.get(), context, -1);
    if (token == LLAMA_TOKEN_NULL) {
      return {OperationStatus::Failed,
              "Constrained chapter generation produced no valid token.", {}};
    }
    if (llama_vocab_is_eog(vocab, token)) break;
    const std::optional<std::string> piece = tokenPiece(vocab, token);
    if (!piece || output.size() + piece->size() > kMaximumJsonBytes) {
      return {OperationStatus::Failed,
              "The generated chapter document exceeded its safe limit.", {}};
    }
    output += *piece;

    llama_seq_id sequence = 0;
    llama_seq_id* sequences[] = {&sequence};
    int32_t sequenceCounts[] = {1};
    llama_pos positions[] = {nPast++};
    int8_t logits[] = {1};
    llama_token tokens[] = {token};
    llama_batch batch{};
    batch.n_tokens = 1;
    batch.token = tokens;
    batch.pos = positions;
    batch.n_seq_id = sequenceCounts;
    batch.seq_id = sequences;
    batch.logits = logits;
    const int32_t decoded = llama_decode(context, batch);
    if (decoded != 0) {
      if (!continueOperation(control)) return interruptedResult(control);
      return {OperationStatus::Failed,
              "SmolVLM2 could not continue chapter generation.", {}};
    }
    if (control.progress && generated % 16 == 15) {
      const double fraction =
          static_cast<double>(generated + 1) / maximumGeneratedTokens;
      control.progress(0.78 + 0.17 * fraction,
                       "Generating video chapters");
    }
  }
  if (output.empty()) {
    return {OperationStatus::Failed,
            "SmolVLM2 returned an empty chapter document.", {}};
  }
  return {OperationStatus::Succeeded, {}, std::move(output)};
}

}  // namespace

struct InferenceEngine::Impl {
  ggml_backend_dev_t device = nullptr;
  bool deviceVerified = false;
};

InferenceEngine::InferenceEngine() : impl_(std::make_unique<Impl>()) {}
InferenceEngine::~InferenceEngine() = default;

CapabilityResult InferenceEngine::inspect(
    const OperationControl& control) {
  if (!continueOperation(control)) {
    const OperationStatus status = interruptionStatus(control);
    return {status == OperationStatus::Yielded ? CapabilityState::Yielded
                                               : CapabilityState::Cancelled,
             {}};
  }
  (void)processRuntime();
  if (impl_->deviceVerified) return {CapabilityState::Ready, {}};
  if (!impl_->device) impl_->device = defaultVulkanDevice();
  if (!impl_->device) {
    return {CapabilityState::Unsupported,
            "No Vulkan device is available for GPU-only chapter "
            "analysis."};
  }
  if (!llama_supports_gpu_offload()) {
    impl_->device = nullptr;
    return {CapabilityState::Unsupported,
            "The pinned llama.cpp runtime cannot offload inference to "
            "Vulkan."};
  }
  ggml_backend_t probe = ggml_backend_dev_init(impl_->device, nullptr);
  if (!probe) {
    impl_->device = nullptr;
    return {CapabilityState::Unsupported,
            "The Vulkan inference device could not be initialized."};
  }
  ggml_backend_free(probe);
  impl_->deviceVerified = true;
  return {CapabilityState::Ready, {}};
}

InferenceResult InferenceEngine::run(const InferenceRequest& request,
                                     const OperationControl& control) {
  const CapabilityResult capability = inspect(control);
  if (capability.state != CapabilityState::Ready) {
    const OperationStatus status =
        capability.state == CapabilityState::Yielded
            ? OperationStatus::Yielded
            : (capability.state == CapabilityState::Cancelled
                   ? OperationStatus::Cancelled
                   : OperationStatus::Unsupported);
    return {status, capability.detail, {}};
  }
  if (!request.imageRgb || request.imageRgb->empty() ||
      request.imageWidth == 0 || request.imageHeight == 0 ||
      request.imageWidth >
          (std::numeric_limits<std::size_t>::max)() / request.imageHeight /
              3u ||
      request.imageRgb->size() !=
          static_cast<std::size_t>(request.imageWidth) *
              request.imageHeight * 3u ||
      request.model.empty() || request.projector.empty() ||
      request.prompt.empty()) {
    return {OperationStatus::Failed,
            "The in-memory chapter inference request is incomplete.", {}};
  }

  std::vector<ggml_backend_dev_t> devices = {impl_->device, nullptr};
  const ggml_backend_buffer_type_t gpuBuffer =
      ggml_backend_dev_buffer_type(impl_->device);
  std::array<llama_model_tensor_buft_override, 2> overrides = {
      llama_model_tensor_buft_override{".*", gpuBuffer},
      llama_model_tensor_buft_override{nullptr, nullptr}};
  LoadProgress modelProgress{&control, 0.58, 0.10,
                             "Loading SmolVLM2 on Vulkan"};
  llama_model_params modelParams = llama_model_default_params();
  modelParams.devices = devices.data();
  modelParams.tensor_buft_overrides = overrides.data();
  modelParams.n_gpu_layers = INT_MAX;
  modelParams.split_mode = LLAMA_SPLIT_MODE_NONE;
  modelParams.main_gpu = 0;
  modelParams.progress_callback = &modelLoadProgress;
  modelParams.progress_callback_user_data = &modelProgress;
  modelParams.use_extra_bufts = false;
  modelParams.no_host = true;
  const std::string modelPath = pathUtf8(request.model);
  ModelPtr model(llama_model_load_from_file(modelPath.c_str(), modelParams),
                 &llama_model_free);
  if (!model) {
    if (!continueOperation(control)) return interruptedResult(control);
    if (modelProgress.callbackFailed.load(std::memory_order_acquire)) {
      return {OperationStatus::Failed,
              "Could not publish chapter model loading progress.", {}};
    }
    return {OperationStatus::Unsupported,
            "SmolVLM2 could not be loaded completely on the selected Vulkan "
            "device; CPU fallback is disabled.",
            {}};
  }

  llama_context_params contextParams = llama_context_default_params();
  contextParams.n_ctx = kContextTokens;
  contextParams.n_batch = kBatchTokens;
  contextParams.n_ubatch = kMicroBatchTokens;
  contextParams.n_seq_max = 1;
  contextParams.n_threads = inferenceThreads();
  contextParams.n_threads_batch = contextParams.n_threads;
  contextParams.abort_callback = &abortInference;
  contextParams.abort_callback_data = const_cast<OperationControl*>(&control);
  contextParams.offload_kqv = true;
  contextParams.op_offload = true;
  ContextPtr context(llama_init_from_model(model.get(), contextParams),
                     &llama_free);
  if (!context) {
    if (!continueOperation(control)) return interruptedResult(control);
    return {OperationStatus::Unsupported,
            "SmolVLM2 could not create a GPU inference context.", {}};
  }
  const std::uint32_t actualContextTokens = llama_n_ctx(context.get());
  if (actualContextTokens == 0) {
    return {OperationStatus::Failed,
            "SmolVLM2 returned an invalid inference context size.", {}};
  }

  if (control.progress) {
    control.progress(0.68, "Loading SmolVLM2 vision projector on Vulkan");
  }
  mtmd_context_params visionParams = mtmd_context_params_default();
  visionParams.use_gpu = true;
  visionParams.print_timings = false;
  visionParams.n_threads = contextParams.n_threads;
  const std::string projectorPath = pathUtf8(request.projector);
  VisionPtr vision(
      mtmd_init_from_file(projectorPath.c_str(), model.get(), visionParams),
      &mtmd_free);
  if (!vision || !mtmd_support_vision(vision.get())) {
    if (!continueOperation(control)) return interruptedResult(control);
    return {OperationStatus::Unsupported,
            "The SmolVLM2 vision projector could not run on Vulkan; CPU "
            "fallback is disabled.",
            {}};
  }
  if (!continueOperation(control)) return interruptedResult(control);

  BitmapPtr bitmap(mtmd_bitmap_init(request.imageWidth, request.imageHeight,
                                    request.imageRgb->data()),
                   &mtmd_bitmap_free);
  const std::optional<std::string> formatted =
      formatPrompt(model.get(), request.prompt);
  ChunksPtr chunks(mtmd_input_chunks_init(), &mtmd_input_chunks_free);
  if (!bitmap || !formatted || !chunks) {
    return {OperationStatus::Failed,
            "Could not prepare the in-memory chapter prompt.", {}};
  }
  mtmd_input_text text{formatted->c_str(), true, true};
  const mtmd_bitmap* bitmaps[] = {bitmap.get()};
  if (mtmd_tokenize(vision.get(), chunks.get(), &text, bitmaps, 1) != 0) {
    return {OperationStatus::Failed,
            "SmolVLM2 could not tokenize the chapter prompt and contact "
            "sheet.",
            {}};
  }
  const std::size_t promptTokens = totalChunkTokens(chunks.get());
  const std::size_t promptPositions = totalChunkPositions(chunks.get());
  if (promptTokens == 0 || promptPositions == 0 ||
      promptPositions >= actualContextTokens) {
    return {OperationStatus::Failed,
            "The chapter prompt does not fit in the fixed inference context.",
            {}};
  }
  if (control.progress) {
    control.progress(0.72, "Encoding video evidence on Vulkan");
  }
  llama_pos nPast = 0;
  if (evaluateChunks(vision.get(), context.get(), chunks.get(), &nPast,
                     kBatchTokens, control) != 0) {
    if (!continueOperation(control)) return interruptedResult(control);
    return {OperationStatus::Failed,
            "SmolVLM2 could not evaluate the chapter prompt.", {}};
  }
  if (!continueOperation(control)) return interruptedResult(control);
  if (nPast < 0 || static_cast<std::uint64_t>(nPast) + 1u >=
                       actualContextTokens) {
    return {OperationStatus::Failed,
            "The evaluated chapter prompt leaves no room for output.", {}};
  }
  const int maximumGeneratedTokens = static_cast<int>(std::min<std::uint64_t>(
      kMaximumGeneratedTokens,
      static_cast<std::uint64_t>(actualContextTokens) -
          static_cast<std::uint64_t>(nPast) - 1u));
  if (control.progress) {
    control.progress(0.78, "Generating video chapters");
  }
  return generateJson(context.get(), llama_model_get_vocab(model.get()),
                      nPast, maximumGeneratedTokens, control);
}

}  // namespace playback_video_chapters
