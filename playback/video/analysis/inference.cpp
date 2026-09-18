#include "playback/video/analysis/inference.h"

#include <ggml-backend.h>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "playback/video/analysis/model.h"
#include "playback/video/decoder.h"
#include "playback/video/frame_conversion.h"

namespace playback_video_analysis {
namespace {

constexpr std::int32_t kBatchTokens = 2048;
constexpr std::int32_t kMicroBatchTokens = 512;
constexpr std::size_t kMaximumGeneratedBytes = 32u * 1024u;
// Storage admission is independent of the native token budget.
constexpr std::size_t kMaximumPromptBytes = 256u * 1024u;

bool cancelled(const OperationControl &control) {
  try {
    return control.cancelled && control.cancelled();
  } catch (...) {
    return true;
  }
}

bool gpuRevoked(const OperationControl &control) {
  try {
    return control.backgroundGpuAllowed && !control.backgroundGpuAllowed();
  } catch (...) {
    return true;
  }
}

bool continueOperation(const OperationControl &control) {
  return !cancelled(control) && !gpuRevoked(control);
}

OperationStatus interruptionStatus(const OperationControl &control) {
  if (cancelled(control))
    return OperationStatus::Cancelled;
  if (gpuRevoked(control))
    return OperationStatus::Yielded;
  return OperationStatus::Failed;
}

bool reportProgress(const OperationControl &control, double progress,
                    std::string phase) {
  try {
    if (control.progress) {
      control.progress(std::clamp(progress, 0.0, 1.0), std::move(phase));
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool backendNameEquals(ggml_backend_dev_t device, std::string_view expected) {
  if (!device)
    return false;
  const ggml_backend_reg_t registry = ggml_backend_dev_backend_reg(device);
  const char *name = registry ? ggml_backend_reg_name(registry) : nullptr;
  if (!name || std::strlen(name) != expected.size())
    return false;
  return std::equal(expected.begin(), expected.end(), name,
                    [](unsigned char left, unsigned char right) {
                      if (left >= 'A' && left <= 'Z')
                        left += 'a' - 'A';
                      if (right >= 'A' && right <= 'Z')
                        right += 'a' - 'A';
                      return left == right;
                    });
}

ggml_backend_dev_t defaultVulkanDevice() {
  // The global registry may expose another backend before Vulkan. Enumerate
  // explicitly and prefer dedicated memory, while still recognizing a real
  // Vulkan iGPU as a GPU device rather than silently falling back to CPU.
  for (const enum ggml_backend_dev_type desired :
       {GGML_BACKEND_DEVICE_TYPE_GPU, GGML_BACKEND_DEVICE_TYPE_IGPU}) {
    const std::size_t count = ggml_backend_dev_count();
    for (std::size_t index = 0; index < count; ++index) {
      ggml_backend_dev_t device = ggml_backend_dev_get(index);
      if (!device || ggml_backend_dev_type(device) != desired ||
          !backendNameEquals(device, "Vulkan")) {
        continue;
      }
      const ggml_backend_buffer_type_t bufferType =
          ggml_backend_dev_buffer_type(device);
      if (bufferType && !ggml_backend_buft_is_host(bufferType))
        return device;
    }
  }
  return nullptr;
}

int inferenceThreads() {
  const unsigned detected = std::thread::hardware_concurrency();
  return static_cast<int>(std::max(1u, std::min(4u, detected ? detected : 1u)));
}

struct LoadProgress {
  const OperationControl *control = nullptr;
  double begin = 0.0;
  double span = 0.0;
  std::atomic<bool> callbackFailed{false};
};

bool modelLoadProgress(float progress, void *opaque) {
  auto *state = static_cast<LoadProgress *>(opaque);
  if (!state || !state->control || !continueOperation(*state->control)) {
    return false;
  }
  if (!reportProgress(*state->control,
                      state->begin +
                          state->span * std::clamp<double>(progress, 0.0, 1.0),
                      "Loading local model on Vulkan")) {
    state->callbackFailed.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool abortInference(void *opaque) {
  const auto *control = static_cast<const OperationControl *>(opaque);
  return !control || !continueOperation(*control);
}

void discardLog(ggml_log_level, const char *, void *) {}

class ProcessRuntime final {
public:
  ProcessRuntime() {
    llama_log_set(&discardLog, nullptr);
    mtmd_helper_log_set(&discardLog, nullptr);
    llama_backend_init();
  }
  ~ProcessRuntime() { llama_backend_free(); }

  ProcessRuntime(const ProcessRuntime &) = delete;
  ProcessRuntime &operator=(const ProcessRuntime &) = delete;
};

ProcessRuntime &processRuntime() {
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

std::string pathUtf8(const std::filesystem::path &path) {
#if defined(_WIN32)
  return wideToUtf8Lossy(path.wstring());
#else
  return path.string();
#endif
}

std::string trimAscii(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(),
              value.end());
  return value;
}

enum class PromptFormatError {
  InvalidInput,
  TooLarge,
  UnsupportedTemplate,
  Failed
};

std::optional<std::string> formatPrompt(const llama_model *model,
                                        const std::string &systemInstruction,
                                        const std::string &userContent,
                                        PromptFormatError *error = nullptr) {
  const auto fail =
      [&](PromptFormatError reason) -> std::optional<std::string> {
    if (error)
      *error = reason;
    return std::nullopt;
  };
  if (!model || userContent.empty())
    return fail(PromptFormatError::InvalidInput);
  if (systemInstruction.size() > kMaximumPromptBytes ||
      userContent.size() > kMaximumPromptBytes - systemInstruction.size())
    return fail(PromptFormatError::TooLarge);
  const char *chatTemplate = llama_model_chat_template(model, nullptr);
  if (!chatTemplate || !*chatTemplate)
    return fail(PromptFormatError::UnsupportedTemplate);
  const std::array<llama_chat_message, 2> messages = {
      llama_chat_message{"system", systemInstruction.c_str()},
      llama_chat_message{"user", userContent.c_str()}};
  const llama_chat_message *firstMessage =
      systemInstruction.empty() ? messages.data() + 1 : messages.data();
  const std::size_t messageCount = systemInstruction.empty() ? 1u : 2u;
  std::vector<char> formatted(std::max<std::size_t>(
      1024, (systemInstruction.size() + userContent.size()) * 2));
  int32_t written = llama_chat_apply_template(
      chatTemplate, firstMessage, messageCount, true, formatted.data(),
      static_cast<int32_t>(std::min<std::size_t>(
          formatted.size(), static_cast<std::size_t>(INT32_MAX))));
  if (written < 0)
    return fail(PromptFormatError::Failed);
  if (static_cast<std::size_t>(written) > kMaximumPromptBytes)
    return fail(PromptFormatError::TooLarge);
  if (static_cast<std::size_t>(written) >= formatted.size()) {
    formatted.resize(static_cast<std::size_t>(written) + 1u);
    written = llama_chat_apply_template(
        chatTemplate, firstMessage, messageCount, true, formatted.data(),
        static_cast<int32_t>(std::min<std::size_t>(
            formatted.size(), static_cast<std::size_t>(INT32_MAX))));
  }
  if (written < 0 || static_cast<std::size_t>(written) >= formatted.size())
    return fail(PromptFormatError::Failed);
  if (static_cast<std::size_t>(written) > kMaximumPromptBytes)
    return fail(PromptFormatError::TooLarge);
  return std::string(formatted.data(), static_cast<std::size_t>(written));
}

std::optional<std::string> tokenPiece(const llama_vocab *vocab,
                                      llama_token token) {
  if (!vocab)
    return std::nullopt;
  std::array<char, 256> local{};
  int32_t length = llama_token_to_piece(
      vocab, token, local.data(), static_cast<int32_t>(local.size()), 0, false);
  if (length >= 0) {
    return std::string(local.data(), static_cast<std::size_t>(length));
  }
  if (length == (std::numeric_limits<int32_t>::min)())
    return std::nullopt;
  const std::size_t required = static_cast<std::size_t>(-length);
  if (required > kMaximumGeneratedBytes)
    return std::nullopt;
  std::vector<char> expanded(required);
  length =
      llama_token_to_piece(vocab, token, expanded.data(),
                           static_cast<int32_t>(expanded.size()), 0, false);
  if (length < 0)
    return std::nullopt;
  return std::string(expanded.data(), static_cast<std::size_t>(length));
}

class BatchOwner final {
public:
  explicit BatchOwner(std::int32_t tokens)
      : batch_(llama_batch_init(tokens, 0, 1)) {}
  ~BatchOwner() { llama_batch_free(batch_); }

  BatchOwner(const BatchOwner &) = delete;
  BatchOwner &operator=(const BatchOwner &) = delete;

  bool valid() const {
    return batch_.token && batch_.pos && batch_.n_seq_id && batch_.seq_id &&
           batch_.logits;
  }
  llama_batch &get() { return batch_; }

private:
  llama_batch batch_{};
};

std::optional<std::vector<llama_token>>
tokenizePrompt(const llama_vocab *vocab, std::string_view prompt) {
  if (!vocab || prompt.empty() || prompt.size() > kMaximumPromptBytes ||
      prompt.size() > static_cast<std::size_t>(INT32_MAX)) {
    return std::nullopt;
  }
  int32_t required =
      llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()),
                     nullptr, 0, true, true);
  if (required == (std::numeric_limits<int32_t>::min)() || required == 0) {
    return std::nullopt;
  }
  if (required < 0)
    required = -required;
  std::vector<llama_token> tokens(static_cast<std::size_t>(required));
  const int32_t written = llama_tokenize(
      vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(),
      static_cast<int32_t>(tokens.size()), true, true);
  if (written <= 0 || written > static_cast<int32_t>(tokens.size())) {
    return std::nullopt;
  }
  tokens.resize(static_cast<std::size_t>(written));
  return tokens;
}

int32_t evaluateText(llama_context *context,
                     const std::vector<llama_token> &tokens, llama_pos *nPast,
                     const OperationControl &control) {
  if (!context || tokens.empty() || !nPast)
    return -1;
  std::size_t offset = 0;
  while (offset < tokens.size()) {
    if (!continueOperation(control))
      return 2;
    const std::int32_t count = static_cast<std::int32_t>(
        std::min<std::size_t>(tokens.size() - offset, kBatchTokens));
    BatchOwner owner(count);
    if (!owner.valid())
      return -1;
    llama_batch &batch = owner.get();
    batch.n_tokens = count;
    for (std::int32_t index = 0; index < count; ++index) {
      batch.token[index] = tokens[offset + static_cast<std::size_t>(index)];
      batch.pos[index] = *nPast + index;
      batch.n_seq_id[index] = 1;
      batch.seq_id[index][0] = 0;
      batch.logits[index] =
          offset + static_cast<std::size_t>(index) + 1u == tokens.size() ? 1
                                                                         : 0;
    }
    const int32_t decoded = llama_decode(context, batch);
    if (decoded != 0)
      return decoded;
    *nPast += count;
    offset += static_cast<std::size_t>(count);
  }
  return 0;
}

int32_t evaluateMultimodalPrompt(mtmd_context *vision, llama_context *context,
                                 const mtmd_input_chunks *chunks,
                                 llama_pos *nPast,
                                 const OperationControl &control) {
  if (!vision || !context || !chunks || !nPast) {
    return -1;
  }
  const std::size_t count = mtmd_input_chunks_size(chunks);
  if (count == 0)
    return -1;
  std::size_t imageCount = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (!continueOperation(control))
      return 2;
    const mtmd_input_chunk *chunk = mtmd_input_chunks_get(chunks, index);
    if (!chunk)
      return -1;
    int32_t evaluated = 0;
    if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
      if (mtmd_encode_chunk(vision, chunk) != 0)
        return -1;
      float *encoded = mtmd_get_output_embd(vision);
      if (!encoded || mtmd_input_chunk_get_n_tokens(chunk) == 0)
        return -1;
      evaluated = mtmd_helper_decode_image_chunk(
          vision, context, chunk, encoded, *nPast, 0, kBatchTokens, nPast,
          nullptr, nullptr);
      ++imageCount;
    } else {
      evaluated = mtmd_helper_eval_chunk_single(vision, context, chunk, *nPast,
                                                0, kBatchTokens,
                                                index + 1u == count, nPast);
    }
    if (evaluated != 0)
      return evaluated;
  }
  return imageCount > 0 ? 0 : -1;
}

enum class SamplingProfile { QwenVision, QwenText };

SamplerPtr createTextSampler(const llama_vocab *vocab, SamplingProfile profile,
                             int generatedTokenBudget,
                             const std::string &outputGrammar) {
  SamplerPtr chain(
      llama_sampler_chain_init(llama_sampler_chain_default_params()),
      &llama_sampler_free);
  if (!chain)
    return {nullptr, &llama_sampler_free};
  if (!outputGrammar.empty()) {
    auto *format =
        llama_sampler_init_grammar(vocab, outputGrammar.c_str(), "root");
    if (!format)
      return {nullptr, &llama_sampler_free};
    llama_sampler_chain_add(chain.get(), format);
  }
  const auto &sampling = profile == SamplingProfile::QwenVision
                             ? kQwenVisionSampling
                             : kQwenTextSampling;
  // Presence penalties apply to generated tokens, not input evidence.
  // The low-level runtime clamps -1 to zero; supply a concrete history bound.
  llama_sampler *penalties = llama_sampler_init_penalties(
      llama_vocab_n_tokens(vocab), generatedTokenBudget, sampling.repeatPenalty,
      sampling.frequencyPenalty, sampling.presencePenalty);
  if (!penalties)
    return {nullptr, &llama_sampler_free};
  llama_sampler_chain_add(chain.get(), penalties);
  llama_sampler_chain_add(chain.get(), llama_sampler_init_top_k(sampling.topK));
  llama_sampler_chain_add(chain.get(),
                          llama_sampler_init_top_p(sampling.topP, 1));
  llama_sampler_chain_add(chain.get(),
                          llama_sampler_init_temp(sampling.temperature));
  // A fixed seed makes identical observations reproducible after resume.
  llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(sampling.seed));
  return chain;
}

struct StageProgress {
  double begin = 0.0;
  double end = 0.0;
  std::string phase;
};

struct GenerationResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::string output;
};

GenerationResult interruptedGeneration(const OperationControl &control) {
  const OperationStatus status = interruptionStatus(control);
  if (status == OperationStatus::Cancelled) {
    return {status, "Video analysis cancelled.", {}};
  }
  if (status == OperationStatus::Yielded) {
    return {status, "Waiting for free GPU memory.", {}};
  }
  return {};
}

GenerationResult
generateText(llama_context *context, const llama_vocab *vocab, llama_pos nPast,
             std::uint32_t contextTokens, int requestedMaximumTokens,
             const StageProgress &progress, const OperationControl &control,
             std::string_view generatorName, SamplingProfile profile,
             const std::string &outputGrammar = {}) {
  if (!context || !vocab || nPast < 0 || requestedMaximumTokens <= 0 ||
      static_cast<std::uint64_t>(nPast) + 1u >= contextTokens) {
    return {OperationStatus::Failed,
            "The model context has no room for output.",
            {}};
  }
  const int maximumTokens = static_cast<int>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(requestedMaximumTokens),
      static_cast<std::uint64_t>(contextTokens) -
          static_cast<std::uint64_t>(nPast) - 1u));
  SamplerPtr sampler =
      createTextSampler(vocab, profile, maximumTokens, outputGrammar);
  if (!sampler) {
    return {OperationStatus::Failed,
            "Could not initialize " + std::string(generatorName) + ".",
            {}};
  }
  std::string output;
  output.reserve(4096);
  bool completed = false;
  for (int generated = 0; generated < maximumTokens; ++generated) {
    if (!continueOperation(control))
      return interruptedGeneration(control);
    const llama_token token = llama_sampler_sample(sampler.get(), context, -1);
    if (token == LLAMA_TOKEN_NULL) {
      return {
          OperationStatus::Failed, "The model produced no valid token.", {}};
    }
    if (llama_vocab_is_eog(vocab, token)) {
      completed = !trimAscii(output).empty();
      break;
    }
    const std::optional<std::string> piece = tokenPiece(vocab, token);
    if (!piece || output.size() + piece->size() > kMaximumGeneratedBytes) {
      return {OperationStatus::Failed,
              "Generated review data exceeded its safe limit.",
              {}};
    }
    output += *piece;

    llama_seq_id sequence = 0;
    llama_seq_id *sequences[] = {&sequence};
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
    if (llama_decode(context, batch) != 0) {
      if (!continueOperation(control))
        return interruptedGeneration(control);
      return {OperationStatus::Failed,
              std::string(generatorName) + " could not continue generation.",
              {}};
    }
    if ((generated & 7) == 7) {
      const double fraction =
          static_cast<double>(generated + 1) / maximumTokens;
      if (!reportProgress(control,
                          progress.begin +
                              (progress.end - progress.begin) * fraction,
                          progress.phase)) {
        return {OperationStatus::Failed,
                "Could not publish video-analysis progress.",
                {}};
      }
    }
  }
  if (!completed) {
    return {OperationStatus::Failed,
            std::string(generatorName) + " did not finish its response.",
            std::move(output)};
  }
  if (!reportProgress(control, progress.end, progress.phase)) {
    return {OperationStatus::Failed,
            "Could not publish video-analysis progress.",
            {}};
  }
  return {OperationStatus::Succeeded, {}, trimAscii(std::move(output))};
}

class LoadedSession final {
public:
  LoadedSession(llama_model *model, llama_context *context,
                mtmd_context *vision, std::uint32_t contextTokens,
                const OperationControl &control)
      : model_(model), context_(context), vision_(vision),
        vocab_(model ? llama_model_get_vocab(model) : nullptr),
        contextTokens_(contextTokens), control_(control) {}

  bool valid() const { return model_ && context_ && vocab_; }

  GenerationResult reviewWindow(
      mtmd_bitmap *video, const playback_video_analysis::ReviewWindow &window,
      const std::vector<int64_t> &frameTimesUs,
      const std::vector<TextCue> &speech, const StageProgress &progress) {
    using namespace playback_video_analysis;
    if (!video || !vision_ || !valid() || !reset())
      return {OperationStatus::Failed,
              "Could not initialize the video review context.",
              {}};
    const std::string content =
        std::string(mtmd_default_marker()) + "\n" +
        escapeReviewEvidence(reviewObservationPrompt(window, speech));
    const auto prompt = formatPrompt(model_, {}, content);
    if (!prompt)
      return {OperationStatus::Failed,
              "The video model has no supported chat template.",
              {}};
    ChunksPtr chunks(mtmd_input_chunks_init(), &mtmd_input_chunks_free);
    mtmd_input_text text{prompt->c_str(), prompt->size(), true, true};
    const mtmd_bitmap *media[] = {video};
    if (!chunks || mtmd_tokenize(vision_, chunks.get(), &text, media, 1) != 0)
      return {OperationStatus::Failed,
              "Could not decode and tokenize the video window.",
              {}};
    constexpr int outputTokens = kReviewObservationTokens;
    if (mtmd_helper_get_n_tokens(chunks.get()) + outputTokens >= contextTokens_)
      return {OperationStatus::Failed,
              "The complete video window exceeds the model context.",
              {}};
    llama_pos nPast = 0;
    if (evaluateMultimodalPrompt(vision_, context_, chunks.get(), &nPast,
                                 control_) != 0)
      return {
          OperationStatus::Failed, "Could not evaluate the video window.", {}};
    const auto grammar = reviewObservationGrammar(window, frameTimesUs);
    if (grammar.empty())
      return {OperationStatus::Failed,
              "The decoded video has no valid observation boundaries.",
              {}};
    return generateText(context_, vocab_, nPast, contextTokens_, outputTokens,
                        progress, control_, "Video observation model",
                        SamplingProfile::QwenVision, grammar);
  }

  struct ReviewTextPage {
    size_t end = 0;
    std::vector<llama_token> tokens;
  };

  std::optional<ReviewTextPage> fitReviewTextPage(
      size_t next, size_t count,
      const std::function<std::string(size_t)> &contentForEnd,
      std::string *error,
      int outputTokens = playback_video_analysis::kReviewAggregationTokens) {
    using namespace playback_video_analysis;
    std::optional<ReviewTextPage> selected;
    size_t low = next + 1;
    size_t high = count;
    while (low <= high) {
      if (!continueOperation(control_))
        return std::nullopt;
      const size_t candidate = low + (high - low) / 2;
      PromptFormatError formatError = PromptFormatError::Failed;
      const auto prompt = formatPrompt(
          model_, {}, escapeReviewEvidence(contentForEnd(candidate)),
          &formatError);
      if (!prompt && formatError == PromptFormatError::TooLarge) {
        high = candidate - 1;
        continue;
      }
      auto tokens = prompt ? tokenizePrompt(vocab_, *prompt) : std::nullopt;
      if (!tokens) {
        *error = "Could not tokenize the chronological review input.";
        return std::nullopt;
      }
      if (tokens->size() + outputTokens + 1u < contextTokens_) {
        selected = ReviewTextPage{candidate, std::move(*tokens)};
        low = candidate + 1;
      } else
        high = candidate - 1;
    }
    if (!selected)
      *error = "One complete review input exceeds the model context.";
    return selected;
  }

  GenerationResult reviewText(const std::vector<llama_token> &tokens,
                              const std::string &grammar, int outputTokens,
                              const StageProgress &progress) {
    if (!continueOperation(control_))
      return interruptedGeneration(control_);
    if (!valid() || grammar.empty() || tokens.empty() || outputTokens <= 0 ||
        tokens.size() + outputTokens + 1u >= contextTokens_)
      return {OperationStatus::Failed,
              "The review text exceeds the model context or has invalid input.",
              {}};
    if (!reset()) {
      if (!continueOperation(control_))
        return interruptedGeneration(control_);
      return {OperationStatus::Failed,
              "Could not reset the video review context.",
              {}};
    }
    llama_pos nPast = 0;
    if (evaluateText(context_, tokens, &nPast, control_) != 0) {
      if (!continueOperation(control_))
        return interruptedGeneration(control_);
      return {OperationStatus::Failed,
              "Could not evaluate the chronological video observations.",
              {}};
    }
    return generateText(context_, vocab_, nPast, contextTokens_, outputTokens,
                        progress, control_, "Video review model",
                        SamplingProfile::QwenText, grammar);
  }

  GenerationResult
  assessReviewEvent(const playback_video_analysis::ReviewValuationInput &event,
                    const std::vector<TextCue> &speech,
                    const StageProgress &progress) {
    using namespace playback_video_analysis;
    std::optional<ReviewValuation> assessment;
    size_t next = 0;
    const auto &observations = event.observations;
    while (next < observations.size()) {
      std::string error;
      const auto page = fitReviewTextPage(
          next, observations.size(),
          [&](size_t end) {
            return reviewValuationPrompt(
                {{observations.begin() + next, observations.begin() + end},
                 next ? std::optional<ReviewEvidence>(observations[next - 1])
                      : event.before,
                 end < observations.size()
                     ? std::optional<ReviewEvidence>(observations[end])
                     : event.after},
                speech);
          },
          &error, kReviewValuationTokens);
      if (!page) {
        if (!continueOperation(control_))
          return interruptedGeneration(control_);
        return {OperationStatus::Failed, std::move(error), {}};
      }
      const auto generated = reviewText(page->tokens, reviewValuationGrammar(),
                                        kReviewValuationTokens, progress);
      if (generated.status != OperationStatus::Succeeded)
        return generated;
      ReviewValuation current;
      if (!parseReviewValuation(generated.output, &current, &error))
        return {OperationStatus::Failed, std::move(error), {}};
      assessment =
          assessment ? combineReviewValuations(*assessment, current) : current;
      next = page->end;
    }
    if (!assessment)
      return {OperationStatus::Failed, "The event has no observations.", {}};
    return {OperationStatus::Succeeded, {}, assessment->rawResponse};
  }

private:
  bool reset() {
    if (!context_ || !vocab_ || !continueOperation(control_))
      return false;
    llama_synchronize(context_);
    llama_memory_t memory = llama_get_memory(context_);
    if (!memory)
      return false;
    llama_memory_clear(memory, false);
    return continueOperation(control_);
  }

  llama_model *model_ = nullptr;
  llama_context *context_ = nullptr;
  mtmd_context *vision_ = nullptr;
  const llama_vocab *vocab_ = nullptr;
  std::uint32_t contextTokens_ = 0;
  const OperationControl &control_;
};

} // namespace

struct InferenceEngine::Impl {
  ggml_backend_dev_t device = nullptr;
  bool deviceVerified = false;
};

InferenceEngine::InferenceEngine() : impl_(std::make_unique<Impl>()) {}
InferenceEngine::~InferenceEngine() = default;

std::optional<playback_video_gpu::MemoryBudget>
InferenceEngine::gpuMemoryBudget() {
  (void)processRuntime();
  const auto device = defaultVulkanDevice();
  if (!device)
    return std::nullopt;
  ggml_backend_dev_props properties{};
  ggml_backend_dev_get_props(device, &properties);
  return properties.device_id
             ? playback_video_gpu::queryMemoryBudget(properties.device_id)
             : std::nullopt;
}

playback_video_analysis::ReviewResult InferenceEngine::reviewVideo(
    const playback_video_analysis::ReviewRequest &request,
    const OperationControl &control,
    const playback_video_analysis::ReviewProgress &completed,
    const std::function<bool(const playback_video_analysis::ReviewProgress &,
                             std::string *)> &checkpoint) {
  using namespace playback_video_analysis;
  ReviewResult result;
  if (!continueOperation(control)) {
    result.status = interruptionStatus(control);
    return result;
  }
  result.progress = completed;
  const auto windows = reviewWindows(request.durationUs);
  if (windows.empty() || request.sourcePath.empty() || request.model.empty() ||
      request.projector.empty() ||
      completed.observations.size() > windows.size()) {
    result.detail = "The video editing review request is invalid.";
    return result;
  }
  if (reviewComplete(request.durationUs, completed)) {
    result.status = OperationStatus::Succeeded;
    return result;
  }
  const auto capability = inspect(control);
  if (capability.state != CapabilityState::Ready) {
    if (capability.state == CapabilityState::Cancelled)
      result.status = OperationStatus::Cancelled;
    else if (capability.state == CapabilityState::Yielded)
      result.status = OperationStatus::Yielded;
    result.detail = capability.detail;
    return result;
  }

  std::vector<ggml_backend_dev_t> devices{impl_->device, nullptr};
  LoadProgress loadProgress{
      &control, reviewProgressFraction(request.durationUs, completed), 0.0};
  auto modelParams = llama_model_default_params();
  modelParams.devices = devices.data();
  modelParams.n_gpu_layers = INT_MAX;
  modelParams.split_mode = LLAMA_SPLIT_MODE_NONE;
  modelParams.progress_callback = &modelLoadProgress;
  modelParams.progress_callback_user_data = &loadProgress;
  modelParams.use_extra_bufts = false;
  modelParams.no_host = true;
  ModelPtr model(
      llama_model_load_from_file(pathUtf8(request.model).c_str(), modelParams),
      &llama_model_free);
  if (!model) {
    result.detail = "Could not load the video editing model on Vulkan.";
    return result;
  }
  auto contextParams = llama_context_default_params();
  contextParams.n_ctx = kReviewContextTokens;
  contextParams.n_batch = kBatchTokens;
  contextParams.n_ubatch = kMicroBatchTokens;
  contextParams.n_threads = inferenceThreads();
  contextParams.n_threads_batch = contextParams.n_threads;
  contextParams.n_seq_max = 1;
  contextParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
  contextParams.abort_callback = &abortInference;
  contextParams.abort_callback_data = const_cast<OperationControl *>(&control);
  ContextPtr context(llama_init_from_model(model.get(), contextParams),
                     &llama_free);
  if (!context) {
    result.detail = "Could not initialize the video review model context.";
    return result;
  }
  if (completed.observations.size() < windows.size()) {
    auto visionParams = mtmd_context_params_default();
    visionParams.use_gpu = true;
    visionParams.device = impl_->device;
    visionParams.n_threads = inferenceThreads();
    visionParams.image_min_tokens = 64;
    visionParams.image_max_tokens = kReviewImageTokens;
    visionParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    VisionPtr vision(mtmd_init_from_file(pathUtf8(request.projector).c_str(),
                                         model.get(), visionParams),
                     &mtmd_free);
    if (!context || !vision || !mtmd_support_vision(vision.get())) {
      result.detail = "Could not initialize native video inference on Vulkan.";
      return result;
    }
    LoadedSession session(model.get(), context.get(), vision.get(),
                          llama_n_ctx(context.get()), control);

    struct VideoInput {
      VideoDecoder decoder;
      const OperationControl *control = nullptr;
      std::vector<int64_t> times;
      std::vector<int64_t> actualTimes;
      VideoFrame frame;
      bool haveFrame = false;

      static int interrupted(void *opaque) {
        return !continueOperation(*static_cast<VideoInput *>(opaque)->control);
      }
      static int next(size_t chunk, void *opaque, mtmd_bitmap **bitmap,
                      char **text) {
        auto &input = *static_cast<VideoInput *>(opaque);
        *bitmap = nullptr;
        *text = nullptr;
        try {
          if (interrupted(opaque))
            return -2;
          if (chunk == 0) {
            *text = _strdup("Video:");
            return *text ? 0 : -2;
          }
          // Qwen's native temporal patches consume consecutive frame pairs.
          // Text timestamps precede each pair, not each independent image.
          const size_t group = (chunk - 1) / 3;
          const size_t part = (chunk - 1) % 3;
          const size_t index = group * 2 + (part == 2 ? 1 : 0);
          if (index >= input.times.size())
            return -1;
          if (part == 0) {
            const auto timestamp =
                "[" + reviewTimestamp(input.times[index]) + "]";
            *text = _strdup(timestamp.c_str());
            return *text ? 0 : -2;
          }
          const int64_t target = input.times[index];
          for (;;) {
            if (input.haveFrame) {
              const int64_t pts = input.frame.timestamp100ns / 10;
              const int64_t duration =
                  std::max<int64_t>(1, input.frame.duration100ns / 10);
              if (pts >= target || (pts <= target && target - pts < duration))
                break;
            }
            if (interrupted(opaque) ||
                !input.decoder.readFrame(input.frame, nullptr, false))
              return -2;
            input.haveFrame = true;
          }
          const int64_t pts =
              std::max<int64_t>(0, input.frame.timestamp100ns / 10);
          if (pts > target + kReviewFrameStepUs)
            return -2;
          VideoFrame pixels;
          playback_video_image::RgbaImage image;
          if (!input.decoder.redecodeLastFrame(pixels) ||
              !playback_video_frame_conversion::toRgba(pixels, &image))
            return -2;
          std::vector<uint8_t> rgb(static_cast<size_t>(image.width) *
                                   image.height * 3);
          for (uint32_t y = 0; y < image.height; ++y) {
            for (uint32_t x = 0; x < image.width; ++x) {
              const auto *source =
                  image.pixels.data() + y * image.strideBytes + x * 4;
              auto *destination =
                  rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
              std::copy_n(source, 3, destination);
            }
          }
          *bitmap = mtmd_bitmap_init(image.width, image.height, rgb.data());
          if (!*bitmap)
            return -2;
          mtmd_bitmap_set_mergeable(*bitmap, true);
          input.actualTimes.push_back(pts);
          return 0;
        } catch (...) {
          return -2;
        }
      }
    } input;
    input.control = &control;
    if (!input.decoder.init(request.sourcePath, &result.detail, true, true,
                            nullptr, request.videoStreamIndex,
                            &VideoInput::interrupted, &input,
                            VideoCpuOutputPrecision::EightBit) ||
        input.decoder.duration100ns() / 10 != request.durationUs) {
      if (result.detail.empty())
        result.detail = "The source video duration changed.";
      return result;
    }
    const double scale =
        std::min(1.0, 1280.0 / std::max(1, input.decoder.width()));
    if (!input.decoder.setTargetSize(
            std::max(2, int(input.decoder.width() * scale) & ~1),
            std::max(2, int(input.decoder.height() * scale) & ~1),
            &result.detail))
      return result;
    for (size_t index = completed.observations.size(); index < windows.size();
         ++index) {
      if (!continueOperation(control)) {
        result.status = interruptionStatus(control);
        return result;
      }
      const auto &window = windows[index];
      input.times = reviewFrameTimes(window);
      input.actualTimes.clear();
      input.haveFrame = false;
      if (!input.decoder.seekToTimestamp100ns(window.startUs * 10)) {
        result.detail = "Could not seek to the next video review window.";
        return result;
      }
      BitmapPtr video(mtmd_bitmap_init_lazy(vision.get(), nullptr, &input,
                                            &VideoInput::next),
                      &mtmd_bitmap_free);
      const std::string phase = "Observing video " + std::to_string(index + 1) +
                                "/" + std::to_string(windows.size()) +
                                " (2 fps)";
      reportProgress(control, 0.8 * index / windows.size(), phase);
      const auto generated = session.reviewWindow(
          video.get(), window, input.actualTimes, request.speech,
          {0.8 * index / windows.size(), 0.8 * (index + 1) / windows.size(),
           phase});
      if (generated.status != OperationStatus::Succeeded) {
        result.status = generated.status;
        result.detail = generated.detail;
        return result;
      }
      ReviewObservation observation{
          window, std::move(input.actualTimes), {}, generated.output};
      if (observation.frameTimesUs.size() != input.times.size() ||
          !parseReviewObservation(generated.output, window,
                                  observation.frameTimesUs,
                                  &observation.activities, &result.detail))
        return result;
      result.progress.observations.push_back(std::move(observation));
      if (checkpoint && !checkpoint(result.progress, &result.detail))
        return result;
    }
  } // Release the video decoder and vision allocations before text aggregation.

  const auto evidence = reviewEvidence(result.progress);
  LoadedSession aggregation(model.get(), context.get(), nullptr,
                            llama_n_ctx(context.get()), control);
  size_t next = result.progress.aggregations.empty()
                    ? 0
                    : result.progress.aggregations.back().evidenceEnd;
  while (next < evidence.size()) {
    if (!continueOperation(control)) {
      result.status = interruptionStatus(control);
      return result;
    }
    const auto fraction =
        reviewProgressFraction(request.durationUs, result.progress);
    const std::string phase = "Grouping events (" + std::to_string(next) + "/" +
                              std::to_string(evidence.size()) +
                              " observations)";
    if (!reportProgress(control, fraction, phase)) {
      result.detail = "Could not publish event aggregation progress.";
      return result;
    }
    // One new observation means one semantic continuity decision. The first
    // row carries the open event. Batching many
    // independent labels in one generation loses their alignment to evidence.
    // Every observation is processed, with actual tokenizer admission.
    const auto input = aggregation.fitReviewTextPage(
        next, next + 1,
        [&](size_t end) {
          return reviewAggregationPrompt(
              reviewAggregationRows(evidence, result.progress, end),
              end == evidence.size(), request.speech);
        },
        &result.detail);
    if (!input) {
      if (!continueOperation(control))
        result.status = interruptionStatus(control);
      return result;
    }
    const size_t end = input->end;
    const auto rows = reviewAggregationRows(evidence, result.progress, end);
    const auto generated = aggregation.reviewText(
        input->tokens, reviewAggregationGrammar(rows), kReviewAggregationTokens,
        {fraction, 0.8 + 0.1 * end / evidence.size(), phase});
    if (generated.status != OperationStatus::Succeeded) {
      result.status = generated.status;
      result.detail = generated.detail;
      result.rawOutput = generated.output;
      return result;
    }
    ReviewAggregation page;
    if (!parseReviewAggregation(generated.output, rows, &page, &result.detail))
      return result;
    result.progress.aggregations.push_back(std::move(page));
    if (checkpoint && !checkpoint(result.progress, &result.detail))
      return result;
    next = end;
  }
  const auto events = reviewEvents(result.progress);
  next = result.progress.valuations.size();
  while (next < events.size()) {
    if (!continueOperation(control)) {
      result.status = interruptionStatus(control);
      return result;
    }
    const auto fraction =
        reviewProgressFraction(request.durationUs, result.progress);
    const std::string phase = "Assessing complete events (" +
                              std::to_string(next) + "/" +
                              std::to_string(events.size()) + ")";
    if (!reportProgress(control, fraction, phase)) {
      result.detail = "Could not publish event assessment progress.";
      return result;
    }
    const auto input = reviewValuationInput(events[next], evidence);
    if (!input) {
      result.detail = "The complete event has invalid evidence boundaries.";
      return result;
    }
    const auto generated = aggregation.assessReviewEvent(
        *input, request.speech,
        {fraction, 0.9 + 0.1 * (next + 1) / events.size(), phase});
    if (generated.status != OperationStatus::Succeeded) {
      result.status = generated.status;
      result.detail = generated.detail;
      result.rawOutput = generated.output;
      return result;
    }
    ReviewValuation valuation;
    if (!parseReviewValuation(generated.output, &valuation, &result.detail))
      return result;
    result.progress.valuations.push_back(std::move(valuation));
    if (checkpoint && !checkpoint(result.progress, &result.detail))
      return result;
    ++next;
  }
  result.status = OperationStatus::Succeeded;
  return result;
}

CapabilityResult InferenceEngine::inspect(const OperationControl &control) {
  if (!continueOperation(control)) {
    const OperationStatus status = interruptionStatus(control);
    return {status == OperationStatus::Yielded ? CapabilityState::Yielded
                                               : CapabilityState::Cancelled,
            {}};
  }
  (void)processRuntime();
  if (impl_->deviceVerified)
    return {CapabilityState::Ready, {}};
  impl_->device = defaultVulkanDevice();
  if (!impl_->device) {
    return {CapabilityState::Unsupported,
            "No Vulkan device is available for GPU-only video analysis."};
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

} // namespace playback_video_analysis
