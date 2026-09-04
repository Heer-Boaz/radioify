#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"
#include "playback/video/chapter/generated_document.h"

namespace playback_video_chapters {

struct InferenceFrame {
  std::uint32_t imageWidth = 0;
  std::uint32_t imageHeight = 0;
  const std::vector<std::uint8_t> *imageRgb = nullptr;
  std::int64_t timeUs = 0;
};

struct InferenceTemporalWindow {
  std::int64_t intervalStartUs = 0;
  std::int64_t intervalEndUs = 0;
  std::vector<InferenceFrame> frames;
};

struct InferenceDialogueCue {
  std::int64_t timeUs = 0;
  std::string text;
};

struct SpeechChapterPlanRequest {
  std::filesystem::path plannerModel;
  std::filesystem::path planAdapter;
  std::int64_t durationUs = 0;
  std::vector<InferenceDialogueCue> englishDialogue;
};

struct SpeechChapterPlanResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::vector<GeneratedChapterPlanEntry> chapterPlan;
};

struct InferenceRequest {
  std::filesystem::path model;
  std::filesystem::path projector;
  std::filesystem::path plannerModel;
  std::filesystem::path chapterPlanAdapter;
  std::int64_t durationUs = 0;
  std::vector<InferenceTemporalWindow> windows;
  std::vector<InferenceDialogueCue> englishDialogue;
  // The ASR adapter supplies candidate boundaries used only to select one
  // visual sample per candidate. The captions-plus-ASR adapter owns the final
  // published boundaries and titles.
  std::vector<GeneratedChapterPlanEntry> chapterPlan;
};

struct InferenceResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  GeneratedDocument document;
};

// Checks the worst-case serialized Chapter-Llama prompt before any expensive
// model is loaded. Temporal observations are generated later, so their published
// aggregate bound is reserved while dialogue is measured exactly.
bool validateInferenceInputBudget(const InferenceRequest &request,
                                  std::string *error = nullptr);

// Caller-owned, model-independent checkpoint for one exact inference request.
// Completed timestamped frame captions survive cooperative GPU
// preemption and form the ownership boundary between the vision model and the
// bounded Chapter-Llama planner;
// native llama/mtmd objects never cross a run() boundary and therefore release
// all GPU allocations immediately when foreground playback reclaims the
// device. Only complete, normalized frame captions cross that boundary.
struct InferenceCheckpoint {
  std::filesystem::path model;
  std::filesystem::path projector;
  std::filesystem::path plannerModel;
  std::filesystem::path chapterPlanAdapter;
  std::int64_t durationUs = 0;
  std::vector<std::vector<std::int64_t>> sampleTimesUs;
  std::vector<std::int64_t> intervalStartsUs;
  std::vector<std::int64_t> intervalEndsUs;
  // One independent frame caption for every evidence item, in chronological
  // order. Checkpoints are committed only after a complete caption.
  std::vector<std::string> observations;
  std::vector<GeneratedChapterPlanEntry> chapterPlan;
};

// Publishes one complete typed stage boundary. Implementations are expected to
// use an atomic replace so a terminated inference worker can resume without
// observing a partially written checkpoint.
using InferenceCheckpointSink =
    std::function<bool(const InferenceCheckpoint &, std::string *error)>;

// Owns the process-wide llama.cpp backend lifecycle. The public boundary stays
// typed and independent of llama.cpp's experimental C structs; all third-party
// ownership is contained by the implementation.
class InferenceEngine final {
public:
  InferenceEngine();
  ~InferenceEngine();

  InferenceEngine(const InferenceEngine &) = delete;
  InferenceEngine &operator=(const InferenceEngine &) = delete;

  CapabilityResult inspect(const OperationControl &control);
  SpeechChapterPlanResult planChaptersFromSpeech(
      const SpeechChapterPlanRequest &request,
      const OperationControl &control);
  InferenceResult run(const InferenceRequest &request,
                      const OperationControl &control,
                      InferenceCheckpoint *checkpoint = nullptr,
                      const InferenceCheckpointSink &checkpointSink = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace playback_video_chapters
