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
  // Optional process-isolated recognizer. Automatic chapter analysis provides
  // this so foreground playback can terminate native GPU work immediately;
  // standalone/manual transcript jobs retain the in-process engine.
  std::function<SpeechWorkerResult(
      const SpeechWorkerRequest &, const std::function<void(int)> &)>
      runSpeechChunk;
};

struct TranscriptOperationResult {
  TranscriptOperationStatus status = TranscriptOperationStatus::Failed;
  std::string detail;
  std::filesystem::path publishedPath;
};

// Stable action identity for Radioify's automatic TranslateToEnglish
// prerequisite. Includes model/configuration and transcript algorithm inputs
// so a persisted derivative is reused only by an equivalent producer.
std::optional<std::string> automaticEnglishTranscriptProducerIdentity(
    std::string *error = nullptr);

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

// Decodes the video's primary audio stream to Whisper's native mono 16 kHz
// format and writes a timestamp-indexed SRT sidecar. The destination is not
// changed unless the complete transcript succeeds.
bool createIndexedTranscript(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    const ProgressCallback &onProgress,
    const std::atomic<bool> *cancelRequested, std::string *error,
    const TranscriptCommitStarted &outputCommitStarted = {},
    std::filesystem::path *publishedPath = nullptr);

} // namespace playback_video_transcript
