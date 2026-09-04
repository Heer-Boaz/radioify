#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/text_evidence.h"

namespace playback_video_chapters {

enum class AnalysisRoute : std::uint8_t {
  // Chapter-Llama's ASR planner selects frame locations, MiniCPM-V captions
  // those frames, and the captions-plus-ASR adapter produces the partition.
  SpeechGuidedCaptionsAndAsr,
};

struct AnalysisRequest {
  std::filesystem::path file;
  int videoStreamIndex = -1;
  std::int64_t durationUs = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
  AnalysisRoute route = AnalysisRoute::SpeechGuidedCaptionsAndAsr;
  std::optional<TextEvidence> englishText;
};

struct OperationControl {
  std::function<bool()> cancelled;
  std::function<bool()> backgroundGpuAllowed;
  std::function<void(std::optional<double>, std::string)> progress;
};

enum class CapabilityState : std::uint8_t {
  Ready,
  SetupRequired,
  Unsupported,
  Yielded,
  Cancelled,
};

struct CapabilityResult {
  CapabilityState state = CapabilityState::Unsupported;
  std::string detail;
};

enum class OperationStatus : std::uint8_t {
  Succeeded,
  Yielded,
  Cancelled,
  Unsupported,
  Failed,
};

struct InstallResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
};

struct AnalysisResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::vector<Chapter> chapters;
  // A completed document remains publishable when an ancillary operation,
  // such as durable cache storage, fails. Warnings are surfaced separately
  // from terminal failure detail so callers never have to infer partial
  // success from an error string.
  std::string warning;
};

class Backend {
public:
  virtual ~Backend() = default;

  virtual std::optional<AnalysisResult>
  cached(const AnalysisRequest &request) = 0;
  virtual CapabilityResult inspect(const AnalysisRequest &request,
                                   const OperationControl &control) = 0;
  virtual InstallResult install(const OperationControl &control) = 0;
  virtual AnalysisResult analyze(const AnalysisRequest &request,
                                 const OperationControl &control) = 0;
};

std::unique_ptr<Backend> createDefaultBackend();

} // namespace playback_video_chapters
