#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/transcript/whisper_engine.h"

namespace playback_video_transcript {

// One fully decoded Whisper chunk crossing the killable helper-process
// boundary. The parent keeps the authoritative PCM buffer and retries this
// exact request after GPU revocation; the worker owns all native model state.
struct SpeechWorkerRequest {
  std::filesystem::path model;
  WhisperAlignmentPreset alignmentPreset = WhisperAlignmentPreset::None;
  std::string sourceLanguage;
  WhisperTask task = WhisperTask::Transcribe;
  const float *samples = nullptr;
  std::size_t sampleCount = 0;
};

struct SpeechWorkerDocument {
  std::string sourceLanguage;
  std::vector<RecognizedSegment> segments;
};

enum class SpeechWorkerStatus : std::uint8_t {
  Succeeded,
  Yielded,
  Cancelled,
  Failed,
};

struct SpeechWorkerResult {
  SpeechWorkerStatus status = SpeechWorkerStatus::Failed;
  std::string detail;
  SpeechWorkerDocument document;
};

} // namespace playback_video_transcript
