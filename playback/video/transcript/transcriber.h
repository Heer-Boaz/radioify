#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "playback/video/transcript/document.h"
#include "playback/video/transcript/worker_types.h"

namespace playback_video_transcript {

struct Progress {
  float fraction = 0.0f;
  std::string phase;
};

using ProgressCallback = std::function<void(const Progress &)>;
using TranscriptCommitStarted = std::function<bool()>;

enum class TranscriptLanguageMode : std::uint8_t {
  SourceLanguage,
  TranslateToEnglish,
};

enum class TranscriptOperationStatus : std::uint8_t {
  Succeeded,
  NoSpeech,
  Yielded,
  Cancelled,
  Failed,
};

struct TranscriptOperationControl {
  std::function<bool()> cancelled;
  std::function<bool()> backgroundGpuAllowed;
  ProgressCallback progress;
  // Optional process-isolated recognizer. Editing analysis provides this
  // so cancellation and GPU pressure can terminate native work immediately;
  // without it, this resumable Whisper operation runs in process. Manual
  // transcripts use the separate Qwen + Whisper worker via createIndexedTranscript.
  std::function<SpeechWorkerResult(
      const SpeechWorkerRequest &, const std::function<void(int)> &)>
      runSpeechChunk;
};

struct TranscriptOperationResult {
  TranscriptOperationStatus status = TranscriptOperationStatus::Failed;
  std::string detail;
  std::filesystem::path publishedPath;
};

// Owns a resumable transcript transaction. GPU revocation may abort the
// current Whisper chunk, but decoded audio and every completed chunk remain
// owner-held so resumption neither seeks approximately nor republishes a
// partial transcript.
class IndexedTranscriptOperation {
public:
  IndexedTranscriptOperation();
  ~IndexedTranscriptOperation();

  IndexedTranscriptOperation(const IndexedTranscriptOperation &) = delete;
  IndexedTranscriptOperation &
  operator=(const IndexedTranscriptOperation &) = delete;

  TranscriptOperationResult
  resume(const std::filesystem::path &videoPath,
         const std::filesystem::path &outputPath,
         TranscriptPublishMode publishMode, TranscriptLanguageMode languageMode,
         const TranscriptOperationControl &control,
         const TranscriptCommitStarted &outputCommitStarted = {});
  void reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Decodes the primary audio stream to mono 16 kHz, recognizes it with Qwen3-ASR
// 1.7B and aligns the words with Whisper large-v3. The destination is not
// changed unless the complete timestamp-indexed SRT succeeds.
bool createIndexedTranscript(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    const ProgressCallback &onProgress,
    const std::atomic<bool> *cancelRequested, std::string *error,
    const TranscriptCommitStarted &outputCommitStarted = {},
    std::filesystem::path *publishedPath = nullptr);

} // namespace playback_video_transcript
