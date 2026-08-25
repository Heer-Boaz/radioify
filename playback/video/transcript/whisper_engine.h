#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace playback_video_transcript {

struct RecognizedSegment {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

class WhisperEngine {
 public:
  static constexpr uint32_t kSampleRate = 16000;

  using ProgressCallback = std::function<void(int)>;
  using AbortCheck = std::function<bool()>;

  WhisperEngine();
  ~WhisperEngine();

  WhisperEngine(const WhisperEngine&) = delete;
  WhisperEngine& operator=(const WhisperEngine&) = delete;

  bool initialize(const std::filesystem::path& modelPath,
                  std::string* deviceDescription, std::string* error);
  bool transcribe(const float* samples, size_t sampleCount,
                  const std::string& prompt,
                  const ProgressCallback& onProgress,
                  const AbortCheck& shouldAbort,
                  std::vector<RecognizedSegment>* segments,
                  std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_transcript
