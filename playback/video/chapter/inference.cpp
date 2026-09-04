#include "playback/video/chapter/inference.h"

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
#include "playback/video/chapter/prompt_contract.h"

namespace playback_video_chapters {
namespace {

constexpr std::uint32_t kContextTokens = 32768;
constexpr std::int32_t kBatchTokens = 2048;
constexpr std::int32_t kMicroBatchTokens = 512;
// Chapter-Llama's published MiniCPM caption extractor uses a 1024-token
// generation budget. Preserve that model contract; the aggregate prompt
// budget below bounds the evidence admitted to the planner.
constexpr int kCaptionTokens = 1024;
constexpr int kChapterPlanTokens = 1024;
constexpr std::size_t kMaximumGeneratedBytes = 32u * 1024u;
constexpr std::size_t kMaximumPromptBytes = 128u * 1024u;
constexpr std::size_t kMaximumDialogueBytes = 600;
// Visual evidence owns a fixed part of the Chapter-Llama planner context. Known
// dialogue and framing are admitted before model loading; generated captions
// must remain inside this aggregate reservation as they are produced.
constexpr std::size_t kMaximumCaptionCorpusPromptBytes = 80u * 1024u;

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

InferenceResult interruptedResult(const OperationControl &control) {
  const OperationStatus status = interruptionStatus(control);
  if (status == OperationStatus::Cancelled) {
    return {status, "Chapter analysis cancelled.", {}};
  }
  if (status == OperationStatus::Yielded) {
    return {status, "Playback reclaimed the GPU.", {}};
  }
  return {};
}

std::optional<InferenceResult>
publishCheckpoint(const InferenceCheckpointSink &sink,
                  const InferenceCheckpoint &checkpoint) {
  if (!sink)
    return std::nullopt;
  std::string detail;
  try {
    if (sink(checkpoint, &detail))
      return std::nullopt;
  } catch (...) {
    detail.clear();
  }
  if (detail.empty())
    detail = "Could not persist chapter-analysis progress.";
  return InferenceResult{OperationStatus::Failed, std::move(detail), {}};
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
                      "Loading chapter model on Vulkan")) {
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
using AdapterPtr =
    std::unique_ptr<llama_adapter_lora, decltype(&llama_adapter_lora_free)>;

std::string pathUtf8(const std::filesystem::path &path) {
#if defined(_WIN32)
  return wideToUtf8Lossy(path.wstring());
#else
  return path.string();
#endif
}

std::size_t occurrenceCount(std::string_view text, std::string_view needle) {
  if (needle.empty())
    return 0;
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::optional<std::string>
buildFrameCaptionPrompt(const InferenceTemporalWindow &window) {
  const char *rawMarker = mtmd_default_marker();
  if (!rawMarker || !*rawMarker || window.frames.size() != 1) {
    return std::nullopt;
  }
  const std::string marker(rawMarker);
  const std::optional<std::string> value =
      buildChapterLlamaMiniCpmV2CaptionPrompt(marker, kMaximumPromptBytes);
  if (!value || occurrenceCount(*value, marker) != 1) {
    return std::nullopt;
  }
  return value;
}

bool captionCorpusWithinBudget(const std::vector<std::string> &observations) {
  std::size_t bytes = 0;
  for (const std::string &observation : observations) {
    const std::size_t escapedBytes =
        escapeChapterLlamaEvidence(observation).size();
    if (escapedBytes > kMaximumCaptionCorpusPromptBytes - bytes)
      return false;
    bytes += escapedBytes;
  }
  return true;
}

std::optional<std::string>
buildSpeechChapterPlanPrompt(const SpeechChapterPlanRequest &request) {
  std::vector<ChapterPromptEvidence> evidence;
  evidence.reserve(request.englishDialogue.size());
  for (const InferenceDialogueCue &cue : request.englishDialogue) {
    evidence.push_back({cue.timeUs, cue.text});
  }
  return buildChapterLlamaSpeechPrompt(request.durationUs, evidence,
                                       kMaximumPromptBytes);
}

std::string trimAscii(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(),
              value.end());
  return value;
}

std::optional<std::string> buildChapterPlanPrompt(
    const InferenceRequest &request,
    const std::vector<std::string> &observations) {
  if (observations.size() != request.windows.size())
    return std::nullopt;
  std::vector<ChapterPromptEvidence> captions;
  std::vector<ChapterPromptEvidence> asr;
  captions.reserve(observations.size());
  asr.reserve(request.englishDialogue.size());
  for (std::size_t index = 0; index < observations.size(); ++index) {
    if (request.windows[index].frames.size() != 1)
      return std::nullopt;
    captions.push_back(
        {request.windows[index].frames.front().timeUs, observations[index]});
  }
  for (const InferenceDialogueCue &cue : request.englishDialogue)
    asr.push_back({cue.timeUs, cue.text});
  return buildChapterLlamaCaptionAsrPrompt(request.durationUs, captions, asr,
                                           kMaximumPromptBytes);
}

std::optional<std::string> formatPrompt(const llama_model *model,
                                        const std::string &systemInstruction,
                                        const std::string &userContent) {
  if (!model || userContent.empty() ||
      systemInstruction.size() + userContent.size() > kMaximumPromptBytes) {
    return std::nullopt;
  }
  const char *chatTemplate = llama_model_chat_template(model, nullptr);
  if (!chatTemplate || !*chatTemplate)
    return std::nullopt;
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
    return std::nullopt;
  if (static_cast<std::size_t>(written) >= formatted.size()) {
    formatted.resize(static_cast<std::size_t>(written) + 1u);
    written = llama_chat_apply_template(
        chatTemplate, firstMessage, messageCount, true, formatted.data(),
        static_cast<int32_t>(std::min<std::size_t>(
            formatted.size(), static_cast<std::size_t>(INT32_MAX))));
  }
  if (written < 0 || static_cast<std::size_t>(written) >= formatted.size() ||
      static_cast<std::size_t>(written) > kMaximumPromptBytes) {
    return std::nullopt;
  }
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
          vision, context, chunk, encoded, *nPast, 0, kBatchTokens, nPast);
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

SamplerPtr createTextSampler() {
  SamplerPtr chain(
      llama_sampler_chain_init(llama_sampler_chain_default_params()),
      &llama_sampler_free);
  if (!chain)
    return {nullptr, &llama_sampler_free};
  llama_sampler *penalties = llama_sampler_init_penalties(64, 1.0f, 0.0f, 0.0f);
  if (!penalties)
    return {nullptr, &llama_sampler_free};
  llama_sampler_chain_add(chain.get(), penalties);
  llama_sampler *greedy = llama_sampler_init_greedy();
  if (!greedy)
    return {nullptr, &llama_sampler_free};
  llama_sampler_chain_add(chain.get(), greedy);
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
    return {status, "Chapter analysis cancelled.", {}};
  }
  if (status == OperationStatus::Yielded) {
    return {status, "Playback reclaimed the GPU.", {}};
  }
  return {};
}

GenerationResult generateText(llama_context *context, const llama_vocab *vocab,
                              llama_pos nPast, std::uint32_t contextTokens,
                              int requestedMaximumTokens,
                              const StageProgress &progress,
                              const OperationControl &control,
                              std::string_view generatorName) {
  if (!context || !vocab || nPast < 0 || requestedMaximumTokens <= 0 ||
      static_cast<std::uint64_t>(nPast) + 1u >= contextTokens) {
    return {OperationStatus::Failed,
            "The chapter planner context has no room for output.",
            {}};
  }
  const int maximumTokens = static_cast<int>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(requestedMaximumTokens),
      static_cast<std::uint64_t>(contextTokens) -
          static_cast<std::uint64_t>(nPast) - 1u));
  SamplerPtr sampler = createTextSampler();
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
      return {OperationStatus::Failed,
              "Chapter-Llama produced no valid token.",
              {}};
    }
    if (llama_vocab_is_eog(vocab, token)) {
      completed = !trimAscii(output).empty();
      break;
    }
    const std::optional<std::string> piece = tokenPiece(vocab, token);
    if (!piece || output.size() + piece->size() > kMaximumGeneratedBytes) {
      return {OperationStatus::Failed,
              "Generated chapter data exceeded its safe limit.",
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
                "Could not publish chapter-analysis progress.",
                {}};
      }
    }
  }
  if (!completed) {
    return {OperationStatus::Failed,
            std::string(generatorName) + " did not finish its response.",
            {}};
  }
  if (!reportProgress(control, progress.end, progress.phase)) {
    return {OperationStatus::Failed,
            "Could not publish chapter-analysis progress.",
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

  GenerationResult observeFrame(std::vector<const mtmd_bitmap *> &bitmaps,
                                const std::string &prompt,
                                const StageProgress &progress) {
    if (bitmaps.size() != 1 || !vision_ || !valid() || !reset()) {
      if (!continueOperation(control_)) {
        return interruptedGeneration(control_);
      }
      return {OperationStatus::Failed,
              "Could not reset the private frame-caption context.",
              {}};
    }
    const char *marker = mtmd_default_marker();
    if (!marker || !*marker ||
        occurrenceCount(prompt, marker) != bitmaps.size()) {
      return {OperationStatus::Failed,
              "The frame-caption request has invalid media markers.",
              {}};
    }
    ChunksPtr chunks(mtmd_input_chunks_init(), &mtmd_input_chunks_free);
    if (!chunks) {
      return {OperationStatus::Failed,
              "Could not prepare the frame-caption request.",
              {}};
    }
    mtmd_input_text text{prompt.c_str(), true, true};
    if (mtmd_tokenize(vision_, chunks.get(), &text, bitmaps.data(),
                      bitmaps.size()) != 0) {
      return {OperationStatus::Failed,
              "The vision-language model could not tokenize a video frame.",
              {}};
    }
    const llama_pos promptPositions = mtmd_helper_get_n_pos(chunks.get());
    if (mtmd_helper_get_n_tokens(chunks.get()) == 0 || promptPositions <= 0 ||
        static_cast<std::uint64_t>(promptPositions) + kCaptionTokens >=
            contextTokens_) {
      return {OperationStatus::Failed,
              "A video frame does not fit the captioner context.",
              {}};
    }
    if (!reportProgress(control_, progress.begin, progress.phase)) {
      return {OperationStatus::Failed,
              "Could not publish chapter-analysis progress.",
              {}};
    }
    llama_pos nPast = 0;
    if (evaluateMultimodalPrompt(vision_, context_, chunks.get(), &nPast,
                                 control_) != 0) {
      if (!continueOperation(control_)) {
        return interruptedGeneration(control_);
      }
      return {OperationStatus::Failed,
              "The vision-language model could not evaluate a video frame.",
              {}};
    }
    return generateText(context_, vocab_, nPast, contextTokens_, kCaptionTokens,
                        progress, control_, "The visual captioner");
  }

  GenerationResult chapterPlanText(const std::string &prompt, int maximumTokens,
                                   const StageProgress &progress) {
    if (!valid() || !reset()) {
      if (!continueOperation(control_)) {
        return interruptedGeneration(control_);
      }
      return {OperationStatus::Failed,
              "Could not reset the private chapter-planning context.",
              {}};
    }
    // Chapter-Llama was trained with this task as a single user message. A
    // Radioify system message here would be an unvalidated distribution shift.
    const std::optional<std::string> formatted =
        formatPrompt(model_, {}, prompt);
    const std::optional<std::vector<llama_token>> tokens =
        formatted ? tokenizePrompt(vocab_, *formatted) : std::nullopt;
    if (!tokens || maximumTokens <= 0 ||
        tokens->size() + static_cast<std::size_t>(maximumTokens) + 1u >=
            contextTokens_) {
      return {OperationStatus::Failed,
              "The complete Chapter-Llama evidence does not fit its "
              "inference context.",
              {}};
    }
    if (!reportProgress(control_, progress.begin, progress.phase)) {
      return {OperationStatus::Failed,
              "Could not publish chapter-analysis progress.",
              {}};
    }
    llama_pos nPast = 0;
    if (evaluateText(context_, *tokens, &nPast, control_) != 0) {
      if (!continueOperation(control_)) {
        return interruptedGeneration(control_);
      }
      return {OperationStatus::Failed,
              "Chapter-Llama could not evaluate the complete timestamped "
              "evidence.",
              {}};
    }
    return generateText(context_, vocab_, nPast, contextTokens_, maximumTokens,
                        progress, control_, "Chapter-Llama");
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

std::vector<std::vector<std::int64_t>>
sampleTimes(const InferenceRequest &request) {
  std::vector<std::vector<std::int64_t>> result;
  result.reserve(request.windows.size());
  for (const InferenceTemporalWindow &window : request.windows) {
    std::vector<std::int64_t> windowTimes;
    windowTimes.reserve(window.frames.size());
    for (const InferenceFrame &frame : window.frames) {
      windowTimes.push_back(frame.timeUs);
    }
    result.push_back(std::move(windowTimes));
  }
  return result;
}

std::vector<std::int64_t> intervalStarts(const InferenceRequest &request) {
  std::vector<std::int64_t> result;
  result.reserve(request.windows.size());
  for (const InferenceTemporalWindow &window : request.windows) {
    result.push_back(window.intervalStartUs);
  }
  return result;
}

std::vector<std::int64_t> intervalEnds(const InferenceRequest &request) {
  std::vector<std::int64_t> result;
  result.reserve(request.windows.size());
  for (const InferenceTemporalWindow &window : request.windows) {
    result.push_back(window.intervalEndUs);
  }
  return result;
}

void initializeCheckpoint(const InferenceRequest &request,
                          InferenceCheckpoint *checkpoint) {
  if (!checkpoint)
    return;
  *checkpoint = {};
  checkpoint->model = request.model;
  checkpoint->projector = request.projector;
  checkpoint->plannerModel = request.plannerModel;
  checkpoint->chapterPlanAdapter = request.chapterPlanAdapter;
  checkpoint->durationUs = request.durationUs;
  checkpoint->sampleTimesUs = sampleTimes(request);
  checkpoint->intervalStartsUs = intervalStarts(request);
  checkpoint->intervalEndsUs = intervalEnds(request);
  checkpoint->chapterPlan = request.chapterPlan;
}

bool sameChapterPlan(const std::vector<GeneratedChapterPlanEntry> &left,
                     const std::vector<GeneratedChapterPlanEntry> &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].startUs != right[index].startUs ||
        left[index].title != right[index].title) {
      return false;
    }
  }
  return true;
}

bool validChapterPlan(const std::vector<GeneratedChapterPlanEntry> &plan,
                      std::int64_t durationUs) {
  if (plan.size() < kMinimumAutomaticChapterCount ||
      plan.size() > kMaximumAutomaticChapterCount || plan.front().startUs != 0)
    return false;
  for (std::size_t index = 0; index < plan.size(); ++index) {
    const GeneratedChapterPlanEntry &entry = plan[index];
    if (entry.startUs < 0 || entry.startUs >= durationUs ||
        entry.title.empty() ||
        entry.title.size() > kMaximumAutomaticTitleBytes ||
        !isValidUtf8(entry.title) ||
        (index > 0 && entry.startUs <= plan[index - 1].startUs)) {
      return false;
    }
  }
  return true;
}

bool checkpointMatches(const InferenceCheckpoint &checkpoint,
                       const InferenceRequest &request) {
  return checkpoint.model == request.model &&
         checkpoint.projector == request.projector &&
         checkpoint.plannerModel == request.plannerModel &&
         checkpoint.chapterPlanAdapter == request.chapterPlanAdapter &&
         checkpoint.durationUs == request.durationUs &&
         checkpoint.sampleTimesUs == sampleTimes(request) &&
         checkpoint.intervalStartsUs == intervalStarts(request) &&
         checkpoint.intervalEndsUs == intervalEnds(request) &&
         sameChapterPlan(checkpoint.chapterPlan, request.chapterPlan);
}

std::optional<std::size_t>
completedObservationWindows(const InferenceRequest &request,
                            std::size_t observationCount) {
  return observationCount <= request.windows.size()
             ? std::optional<std::size_t>(observationCount)
             : std::nullopt;
}

bool validCheckpoint(const InferenceCheckpoint &checkpoint,
                     const InferenceRequest &request) {
  const std::size_t expectedObservations = request.windows.size();
  if (!validChapterPlan(checkpoint.chapterPlan, request.durationUs) ||
      !sameChapterPlan(checkpoint.chapterPlan, request.chapterPlan)) {
    return false;
  }
  if (checkpoint.observations.size() > expectedObservations ||
      !completedObservationWindows(request, checkpoint.observations.size())) {
    return false;
  }
  for (const std::string &observation : checkpoint.observations) {
    if (observation.empty() ||
        observation.size() > kMaximumAutomaticCaptionBytes ||
        !isValidUtf8(observation)) {
      return false;
    }
  }
  if (!captionCorpusWithinBudget(checkpoint.observations))
    return false;
  if (checkpoint.observations.size() != expectedObservations) {
    return true;
  }
  return true;
}

bool validRequest(const InferenceRequest &request) {
  if (request.model.empty() || request.projector.empty() ||
      request.plannerModel.empty() || request.chapterPlanAdapter.empty() ||
      request.englishDialogue.empty() ||
      request.durationUs < kMinimumAutomaticChapterVideoDurationUs ||
      request.durationUs > kMaximumAutomaticChapterVideoDurationUs ||
      request.windows.size() < kMinimumAutomaticEvidenceSampleCount ||
      request.windows.size() > kMaximumAutomaticEvidenceSampleCount ||
      request.windows.size() != request.chapterPlan.size() ||
      !validChapterPlan(request.chapterPlan, request.durationUs)) {
    return false;
  }
  std::vector<std::int64_t> expectedSampleTimesUs;
  expectedSampleTimesUs.reserve(request.chapterPlan.size());
  for (const GeneratedChapterPlanEntry &chapter : request.chapterPlan)
    expectedSampleTimesUs.push_back(chapter.startUs);
  expectedSampleTimesUs.front() =
      std::min<std::int64_t>(1'000'000, request.durationUs - 1);
  std::int64_t previousTimeUs = -1;
  std::int64_t previousEndUs = 0;
  for (std::size_t index = 0; index < request.windows.size(); ++index) {
    const InferenceTemporalWindow &window = request.windows[index];
    const std::int64_t expectedStartUs =
        index == 0
            ? 0
            : expectedSampleTimesUs[index - 1] +
                  (expectedSampleTimesUs[index] -
                   expectedSampleTimesUs[index - 1]) /
                      2;
    const std::int64_t expectedEndUs =
        index + 1 == expectedSampleTimesUs.size()
            ? request.durationUs
            : expectedSampleTimesUs[index] +
                  (expectedSampleTimesUs[index + 1] -
                   expectedSampleTimesUs[index]) /
                      2;
    if (window.frames.size() != 1 ||
        window.frames.front().timeUs != expectedSampleTimesUs[index] ||
        window.intervalStartUs != expectedStartUs ||
        window.intervalEndUs != expectedEndUs ||
        window.intervalStartUs != previousEndUs ||
        window.intervalEndUs <= window.intervalStartUs ||
        window.intervalEndUs > request.durationUs ||
        (index + 1 == request.windows.size() &&
         window.intervalEndUs != request.durationUs) ||
        (index + 1 < request.windows.size() &&
         window.intervalEndUs > request.windows[index + 1].intervalStartUs)) {
      return false;
    }
    for (const InferenceFrame &frame : window.frames) {
      if (!frame.imageRgb || frame.imageRgb->empty() || frame.imageWidth == 0 ||
          frame.imageHeight == 0 ||
          frame.imageWidth > (std::numeric_limits<std::size_t>::max)() /
                                 frame.imageHeight / 3u ||
          frame.imageRgb->size() != static_cast<std::size_t>(frame.imageWidth) *
                                        frame.imageHeight * 3u ||
          frame.timeUs <= previousTimeUs ||
          frame.timeUs < window.intervalStartUs ||
          frame.timeUs >= window.intervalEndUs) {
        return false;
      }
      previousTimeUs = frame.timeUs;
    }
    previousEndUs = window.intervalEndUs;
  }
  std::int64_t previousDialogueUs = -1;
  for (const InferenceDialogueCue &cue : request.englishDialogue) {
    if (cue.timeUs < 0 || cue.timeUs >= request.durationUs ||
        cue.timeUs < previousDialogueUs || cue.text.empty() ||
        cue.text.size() > kMaximumDialogueBytes || !isValidUtf8(cue.text)) {
      return false;
    }
    previousDialogueUs = cue.timeUs;
  }
  return true;
}

} // namespace

bool validateInferenceInputBudget(const InferenceRequest &request,
                                  std::string *error) {
  // Serialize the exact known input (timestamps, framing, and required
  // dialogue), then reserve a separately enforced aggregate budget for the
  // captions that MiniCPM-V will generate. This admits useful long-form
  // evidence without pretending every concise caption reaches its per-item
  // safety cap.
  if (request.englishDialogue.empty()) {
    if (error)
      *error = "Chapter inference requires an English timecoded transcript.";
    return false;
  }
  const std::vector<std::string> observations(request.windows.size(), "");
  const std::optional<std::string> prompt =
      buildChapterPlanPrompt(request, observations);
  if (prompt && prompt->size() <=
                    kMaximumPromptBytes - kMaximumCaptionCorpusPromptBytes)
    return true;
  if (error) {
    *error = "The timestamped captions and ASR evidence exceed the bounded "
             "Chapter-Llama context. Shorten the source or use "
             "a less dense subtitle track.";
  }
  return false;
}

struct InferenceEngine::Impl {
  ggml_backend_dev_t device = nullptr;
  bool deviceVerified = false;
};

InferenceEngine::InferenceEngine() : impl_(std::make_unique<Impl>()) {}
InferenceEngine::~InferenceEngine() = default;

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
            "No Vulkan device is available for GPU-only chapter analysis."};
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

SpeechChapterPlanResult InferenceEngine::planChaptersFromSpeech(
    const SpeechChapterPlanRequest &request,
    const OperationControl &control) {
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
  if (request.plannerModel.empty() || request.planAdapter.empty() ||
      request.durationUs < kMinimumAutomaticChapterVideoDurationUs ||
      request.durationUs > kMaximumAutomaticChapterVideoDurationUs ||
      request.englishDialogue.empty()) {
    return {OperationStatus::Failed,
            "The speech-guided chapter-plan request is incomplete.",
            {}};
  }
  std::int64_t previousCueUs = -1;
  for (const InferenceDialogueCue &cue : request.englishDialogue) {
    if (cue.timeUs < 0 || cue.timeUs >= request.durationUs ||
        cue.timeUs < previousCueUs || cue.text.empty() ||
        cue.text.size() > kMaximumDialogueBytes || !isValidUtf8(cue.text)) {
      return {OperationStatus::Failed,
              "The speech-guided chapter-plan transcript is invalid.",
              {}};
    }
    previousCueUs = cue.timeUs;
  }
  const std::optional<std::string> prompt =
      buildSpeechChapterPlanPrompt(request);
  if (!prompt) {
    return {OperationStatus::Unsupported,
            "The English transcript exceeds the bounded Chapter-Llama "
            "planner context.",
            {}};
  }

  std::vector<ggml_backend_dev_t> devices = {impl_->device, nullptr};
  const ggml_backend_buffer_type_t gpuBuffer =
      ggml_backend_dev_buffer_type(impl_->device);
  std::array<llama_model_tensor_buft_override, 2> overrides = {
      llama_model_tensor_buft_override{".*", gpuBuffer},
      llama_model_tensor_buft_override{nullptr, nullptr}};
  LoadProgress loadProgress{&control, 0.02, 0.45};
  llama_model_params modelParams = llama_model_default_params();
  modelParams.devices = devices.data();
  modelParams.tensor_buft_overrides = overrides.data();
  modelParams.n_gpu_layers = INT_MAX;
  modelParams.split_mode = LLAMA_SPLIT_MODE_NONE;
  modelParams.main_gpu = 0;
  modelParams.progress_callback = &modelLoadProgress;
  modelParams.progress_callback_user_data = &loadProgress;
  modelParams.use_extra_bufts = false;
  modelParams.no_host = true;

  const std::string modelPath = pathUtf8(request.plannerModel);
  ModelPtr model(llama_model_load_from_file(modelPath.c_str(), modelParams),
                 &llama_model_free);
  if (!model) {
    if (!continueOperation(control)) {
      const InferenceResult interrupted = interruptedResult(control);
      return {interrupted.status, interrupted.detail, {}};
    }
    return {OperationStatus::Unsupported,
            "The Chapter-Llama planner model could not be loaded completely "
            "on Vulkan; CPU fallback is disabled.",
            {}};
  }

  llama_context_params contextParams = llama_context_default_params();
  contextParams.n_ctx =
      std::min(kContextTokens, static_cast<std::uint32_t>(std::max(
                                   1, llama_model_n_ctx_train(model.get()))));
  contextParams.n_batch = kBatchTokens;
  contextParams.n_ubatch = kMicroBatchTokens;
  contextParams.n_seq_max = 1;
  contextParams.n_threads = inferenceThreads();
  contextParams.n_threads_batch = contextParams.n_threads;
  contextParams.abort_callback = &abortInference;
  contextParams.abort_callback_data = const_cast<OperationControl *>(&control);
  contextParams.offload_kqv = true;
  contextParams.op_offload = true;
  ContextPtr context(llama_init_from_model(model.get(), contextParams),
                     &llama_free);
  if (!context) {
    if (!continueOperation(control)) {
      const InferenceResult interrupted = interruptedResult(control);
      return {interrupted.status, interrupted.detail, {}};
    }
    return {OperationStatus::Unsupported,
            "Chapter-Llama could not create the planner GPU context.",
            {}};
  }
  LoadedSession session(model.get(), context.get(), nullptr,
                        llama_n_ctx(context.get()), control);
  if (!session.valid()) {
    return {OperationStatus::Failed,
            "The Chapter-Llama planner session is incomplete.",
            {}};
  }

  const std::string adapterPath = pathUtf8(request.planAdapter);
  AdapterPtr adapter(llama_adapter_lora_init(model.get(), adapterPath.c_str()),
                     &llama_adapter_lora_free);
  if (!adapter ||
      llama_set_adapter_lora(context.get(), adapter.get(), 1.0f) != 0) {
    return {OperationStatus::Unsupported,
            "The verified Chapter-Llama ASR plan adapter could not be "
            "applied.",
            {}};
  }
  const GenerationResult generated = session.chapterPlanText(
      *prompt, kChapterPlanTokens,
      {0.50, 0.98, "Planning chapters from the English transcript"});
  llama_clear_adapter_lora(context.get());
  if (generated.status != OperationStatus::Succeeded)
    return {generated.status, generated.detail, {}};

  std::vector<GeneratedChapterPlanEntry> plan;
  std::string parseError;
  if (!parseChapterLlamaPlan(generated.output, request.durationUs, &plan,
                             &parseError)) {
    return {OperationStatus::Failed,
            "The Chapter-Llama ASR planner returned an invalid plan: " +
                parseError,
            {}};
  }
  SpeechChapterPlanResult result;
  result.status = OperationStatus::Succeeded;
  result.chapterPlan = std::move(plan);
  return result;
}

InferenceResult
InferenceEngine::run(const InferenceRequest &request,
                     const OperationControl &control,
                     InferenceCheckpoint *checkpoint,
                     const InferenceCheckpointSink &checkpointSink) {
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
  if (!validRequest(request)) {
    return {OperationStatus::Failed,
            "The typed in-memory chapter inference request is incomplete.",
            {}};
  }
  std::string budgetError;
  if (!validateInferenceInputBudget(request, &budgetError)) {
    return {OperationStatus::Unsupported, std::move(budgetError), {}};
  }

  InferenceCheckpoint localCheckpoint;
  InferenceCheckpoint *activeCheckpoint =
      checkpoint ? checkpoint : &localCheckpoint;
  if (!checkpointMatches(*activeCheckpoint, request)) {
    initializeCheckpoint(request, activeCheckpoint);
  }
  if (!validCheckpoint(*activeCheckpoint, request)) {
    return {OperationStatus::Failed,
            "The private chapter inference checkpoint is invalid.",
            {}};
  }

  std::vector<ggml_backend_dev_t> devices = {impl_->device, nullptr};
  const ggml_backend_buffer_type_t gpuBuffer =
      ggml_backend_dev_buffer_type(impl_->device);
  std::array<llama_model_tensor_buft_override, 2> overrides = {
      llama_model_tensor_buft_override{".*", gpuBuffer},
      llama_model_tensor_buft_override{nullptr, nullptr}};
  llama_model_params modelParams = llama_model_default_params();
  modelParams.devices = devices.data();
  modelParams.tensor_buft_overrides = overrides.data();
  modelParams.n_gpu_layers = INT_MAX;
  modelParams.split_mode = LLAMA_SPLIT_MODE_NONE;
  modelParams.main_gpu = 0;
  modelParams.progress_callback = &modelLoadProgress;
  modelParams.use_extra_bufts = false;
  modelParams.no_host = true;
  llama_context_params baseContextParams = llama_context_default_params();
  baseContextParams.n_ctx = kContextTokens;
  baseContextParams.n_batch = kBatchTokens;
  baseContextParams.n_ubatch = kMicroBatchTokens;
  baseContextParams.n_seq_max = 1;
  baseContextParams.n_threads = inferenceThreads();
  baseContextParams.n_threads_batch = baseContextParams.n_threads;
  baseContextParams.abort_callback = &abortInference;
  baseContextParams.abort_callback_data =
      const_cast<OperationControl *>(&control);
  baseContextParams.offload_kqv = true;
  baseContextParams.op_offload = true;

  // An observation-complete checkpoint is the durable boundary between the
  // two model owners. Resume directly with Chapter-Llama instead of loading
  // MiniCPM-V and its projector only to release them again.
  const std::size_t expectedObservations = request.windows.size();
  if (activeCheckpoint->observations.size() < expectedObservations) {
    LoadProgress loadProgress{&control, 0.58, 0.10};
    modelParams.progress_callback_user_data = &loadProgress;
    const std::string modelPath = pathUtf8(request.model);
    ModelPtr model(llama_model_load_from_file(modelPath.c_str(), modelParams),
                   &llama_model_free);
    if (!model) {
      if (!continueOperation(control))
        return interruptedResult(control);
      if (loadProgress.callbackFailed.load(std::memory_order_acquire)) {
        return {OperationStatus::Failed,
                "Could not publish chapter model loading progress.",
                {}};
      }
      return {OperationStatus::Unsupported,
              "The chapter model could not be loaded completely on Vulkan; "
              "CPU fallback is disabled.",
              {}};
    }

    llama_context_params contextParams = baseContextParams;
    contextParams.n_ctx =
        std::min(kContextTokens, static_cast<std::uint32_t>(std::max(
                                     1, llama_model_n_ctx_train(model.get()))));
    ContextPtr context(llama_init_from_model(model.get(), contextParams),
                       &llama_free);
    if (!context) {
      if (!continueOperation(control))
        return interruptedResult(control);
      return {OperationStatus::Unsupported,
              "The chapter model could not create a GPU inference context.",
              {}};
    }
    const std::uint32_t actualContextTokens = llama_n_ctx(context.get());
    if (actualContextTokens == 0) {
      return {OperationStatus::Failed,
              "The chapter model returned an invalid inference context size.",
              {}};
    }

    if (!reportProgress(control, 0.68,
                        "Loading chapter vision projector on Vulkan")) {
      return {OperationStatus::Failed,
              "Could not publish chapter-analysis progress.",
              {}};
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
      if (!continueOperation(control))
        return interruptedResult(control);
      return {OperationStatus::Unsupported,
              "The chapter vision projector could not run on Vulkan; CPU "
              "fallback is disabled.",
              {}};
    }
    if (!mtmd_vision_backend_is_gpu(vision.get())) {
      const char *backend = mtmd_get_vision_backend_name(vision.get());
      std::string detail =
          "The chapter vision projector selected a CPU backend";
      if (backend && *backend)
        detail += " (" + std::string(backend) + ")";
      detail += "; GPU-only chapter analysis cannot continue.";
      return {OperationStatus::Unsupported, std::move(detail), {}};
    }

    LoadedSession session(model.get(), context.get(), vision.get(),
                          actualContextTokens, control);
    if (!session.valid()) {
      return {OperationStatus::Failed,
              "The chapter inference session is incomplete.",
              {}};
    }
    constexpr double kCaptionBegin = 0.70;
    constexpr double kCaptionEnd = 0.92;
    const std::optional<std::size_t> firstWindow = completedObservationWindows(
        request, activeCheckpoint->observations.size());
    if (!firstWindow) {
      return {OperationStatus::Failed,
              "The temporal-observation checkpoint exceeds the window count.",
              {}};
    }
    for (std::size_t index = *firstWindow; index < request.windows.size();
         ++index) {
      if (!continueOperation(control))
        return interruptedResult(control);
      const InferenceTemporalWindow &window = request.windows[index];
      std::vector<BitmapPtr> ownedBitmaps;
      std::vector<const mtmd_bitmap *> bitmaps;
      ownedBitmaps.reserve(window.frames.size());
      bitmaps.reserve(window.frames.size());
      for (const InferenceFrame &frame : window.frames) {
        BitmapPtr bitmap(mtmd_bitmap_init(frame.imageWidth, frame.imageHeight,
                                          frame.imageRgb->data()),
                         &mtmd_bitmap_free);
        if (!bitmap) {
          return {OperationStatus::Failed,
                  "Could not prepare a temporal video frame.",
                  {}};
        }
        bitmaps.push_back(bitmap.get());
        ownedBitmaps.push_back(std::move(bitmap));
      }
      const std::optional<std::string> captionPrompt =
          buildFrameCaptionPrompt(window);
      if (!captionPrompt) {
        return {OperationStatus::Failed,
                "Could not construct the frame-caption request.",
                {}};
      }
      const double begin = kCaptionBegin + (kCaptionEnd - kCaptionBegin) *
                                               static_cast<double>(index) /
                                               request.windows.size();
      const double end = kCaptionBegin + (kCaptionEnd - kCaptionBegin) *
                                             static_cast<double>(index + 1) /
                                             request.windows.size();
      const std::string phase = "Captioning frame " +
                                std::to_string(index + 1) + " of " +
                                std::to_string(request.windows.size());
      const GenerationResult generated =
          session.observeFrame(bitmaps, *captionPrompt, {begin, end, phase});
      if (generated.status != OperationStatus::Succeeded) {
        return {generated.status, generated.detail, {}};
      }
      std::string caption;
      std::string parseError;
      if (!normalizeGeneratedFrameCaption(generated.output, &caption,
                                          &parseError)) {
        return {OperationStatus::Failed,
                "The visual captioner returned invalid text for frame " +
                    std::to_string(index + 1) + ": " + parseError,
                {}};
      }
      activeCheckpoint->observations.push_back(std::move(caption));
      if (!captionCorpusWithinBudget(activeCheckpoint->observations)) {
        return {OperationStatus::Unsupported,
                "The generated complete-timeline captions exceed the "
                "bounded Chapter-Llama context.",
                {}};
      }
      if (const auto failure =
              publishCheckpoint(checkpointSink, *activeCheckpoint)) {
        return *failure;
      }
    }
  }

  // Vision and chapter planning are separate model owners. The MiniCPM-V
  // scope above ends before the text model is allocated, so their Vulkan
  // resources never overlap. The ASR plan selected these frames; the official
  // captions-plus-ASR adapter below owns the publishable boundaries and titles.
  if (!continueOperation(control))
    return interruptedResult(control);

  LoadProgress plannerLoadProgress{&control, 0.92, 0.02};
  modelParams.progress_callback_user_data = &plannerLoadProgress;
  const std::string plannerModelPath = pathUtf8(request.plannerModel);
  ModelPtr plannerModel(
      llama_model_load_from_file(plannerModelPath.c_str(), modelParams),
      &llama_model_free);
  if (!plannerModel) {
    if (!continueOperation(control))
      return interruptedResult(control);
    return {OperationStatus::Unsupported,
            "The Chapter-Llama base model could not be loaded completely on "
            "Vulkan; CPU fallback is disabled.",
            {}};
  }
  const std::uint32_t plannerTrainingContext = static_cast<std::uint32_t>(
      std::max(1, llama_model_n_ctx_train(plannerModel.get())));
  llama_context_params plannerContextParams = baseContextParams;
  plannerContextParams.n_ctx = std::min(kContextTokens, plannerTrainingContext);
  ContextPtr plannerContext(
      llama_init_from_model(plannerModel.get(), plannerContextParams),
      &llama_free);
  if (!plannerContext) {
    if (!continueOperation(control))
      return interruptedResult(control);
    return {OperationStatus::Unsupported,
            "Chapter-Llama could not create a GPU inference context.",
            {}};
  }
  const std::uint32_t plannerContextTokens = llama_n_ctx(plannerContext.get());
  if (plannerContextTokens == 0) {
    return {OperationStatus::Failed,
            "Chapter-Llama returned an invalid context size.",
            {}};
  }
  auto session =
      std::make_unique<LoadedSession>(plannerModel.get(), plannerContext.get(),
                                      nullptr, plannerContextTokens, control);
  if (!session->valid()) {
    return {OperationStatus::Failed,
            "The Chapter-Llama inference session is incomplete.",
            {}};
  }

  const std::string adapterPath = pathUtf8(request.chapterPlanAdapter);
  AdapterPtr adapter(
      llama_adapter_lora_init(plannerModel.get(), adapterPath.c_str()),
      &llama_adapter_lora_free);
  if (!adapter ||
      llama_set_adapter_lora(plannerContext.get(), adapter.get(), 1.0f) != 0) {
    return {OperationStatus::Unsupported,
            "The verified Chapter-Llama captions-plus-ASR adapter could not "
            "be applied.",
            {}};
  }
  const std::optional<std::string> prompt =
      buildChapterPlanPrompt(request, activeCheckpoint->observations);
  if (!prompt) {
    llama_clear_adapter_lora(plannerContext.get());
    return {OperationStatus::Unsupported,
            "The complete timestamped captions and ASR transcript do not fit "
            "the Chapter-Llama context.",
            {}};
  }
  const GenerationResult generated = session->chapterPlanText(
      *prompt, kChapterPlanTokens,
      {0.94, 0.995, "Planning chapters from captions and transcript"});
  llama_clear_adapter_lora(plannerContext.get());
  if (generated.status != OperationStatus::Succeeded)
    return {generated.status, generated.detail, {}};

  std::vector<GeneratedChapterPlanEntry> finalPlan;
  std::string parseError;
  if (!parseChapterLlamaPlan(generated.output, request.durationUs, &finalPlan,
                             &parseError)) {
    return {OperationStatus::Failed,
            "The Chapter-Llama captions-plus-ASR planner returned an invalid "
            "plan: " +
                parseError,
            {}};
  }
  GeneratedDocument document;
  document.chapters = std::move(finalPlan);
  return {OperationStatus::Succeeded, {}, std::move(document)};
}

} // namespace playback_video_chapters
