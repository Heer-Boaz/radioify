#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "playback/video/transcript/whisper_model_config.h"

namespace playback_video_transcript {

enum class WhisperTask : std::uint8_t {
  Transcribe,
  TranslateToEnglish,
};

struct RecognizedToken {
  int64_t startUs = 0;
  int64_t endUs = 0;
  // DTW alignment point inside the spoken token, or -1 when the selected
  // model has no known whisper.cpp alignment-head preset.
  int64_t alignmentUs = -1;
  std::string text;
};

struct RecognizedSegment {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
  // Lexical tokens only. Whisper special/timestamp tokens never cross this
  // boundary. Valid token timing lets the subtitle policy form readable cues
  // without guessing from a decoder-sized segment.
  std::vector<RecognizedToken> tokens;
};

class WhisperEngine {
public:
  static constexpr uint32_t kSampleRate = 16000;

  using ProgressCallback = std::function<void(int)>;
  using AbortCheck = std::function<bool()>;

  WhisperEngine();
  ~WhisperEngine();

  WhisperEngine(const WhisperEngine &) = delete;
  WhisperEngine &operator=(const WhisperEngine &) = delete;

  bool initialize(const std::filesystem::path &modelPath,
                  WhisperAlignmentPreset alignmentPreset, WhisperTask task,
                  std::string sourceLanguage, std::string *deviceDescription,
                  std::string *error);
  bool transcribe(const float *samples, size_t sampleCount,
                  const ProgressCallback &onProgress,
                  const AbortCheck &shouldAbort,
                  std::vector<RecognizedSegment> *segments, std::string *error);
  // Returns the explicit or first successfully detected ISO 639-1 language.
  // The value becomes stable after the first chunk containing speech.
  std::string sourceLanguage() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace playback_video_transcript
