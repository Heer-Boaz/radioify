#include "playback/video/analysis/inference_worker_protocol.h"
#include "playback/video/analysis/inference_worker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "playback/video/analysis/edit_review_store.h"

namespace playback_video_analysis {
namespace {

constexpr std::size_t kMaximumSpeechSamples =
    60ull * playback_video_transcript::WhisperEngine::kSampleRate;

std::atomic<std::uint64_t> gStagingSequence{0};

const std::filesystem::path kRequestName = L"request.json";
const std::filesystem::path kAudioName = L"audio.f32";
const std::filesystem::path kProgressName = L"progress.json";
const std::filesystem::path kResultName = L"result.json";

struct OwnedRequest {
  enum class Operation {
    TranscribeSpeech,
    ReviewVideo,
  } operation = Operation::TranscribeSpeech;
  playback_video_analysis::ReviewRequest reviewRequest;
  playback_video_transcript::SpeechWorkerRequest speechRequest;
  std::vector<float> speechSamples;
};

std::string pathUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}

std::filesystem::path pathFromUtf8(const std::string &value) {
  return std::filesystem::u8path(value);
}

void setError(std::string *error, std::string value) {
  if (error)
    *error = std::move(value);
}

std::filesystem::path stagingPath(const std::filesystem::path &destination) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path staging = destination;
  staging +=
      L".partial-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(ticks) + L"-" +
      std::to_wstring(gStagingSequence.fetch_add(1, std::memory_order_relaxed));
  return staging;
}

bool publish(const std::filesystem::path &staging,
             const std::filesystem::path &destination, std::string *error) {
  if (MoveFileExW(staging.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  setError(error, "Could not atomically publish an analysis worker file "
                  "(Windows error " +
                      std::to_string(GetLastError()) + ").");
  std::error_code ignored;
  std::filesystem::remove(staging, ignored);
  return false;
}

bool writeJson(const std::filesystem::path &path,
               const nlohmann::json &document, std::string *error) {
  const std::filesystem::path staging = stagingPath(path);
  std::ofstream output(staging, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create an analysis worker staging file.");
    return false;
  }
  output << document.dump() << '\n';
  output.flush();
  if (!output) {
    output.close();
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    setError(error, "Could not finish an analysis worker staging file.");
    return false;
  }
  output.close();
  return publish(staging, path, error);
}

std::optional<nlohmann::json> readJson(const std::filesystem::path &path,
                                       std::string *error) {
  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    setError(error, "An analysis worker input file is missing.");
    return std::nullopt;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
  if (filesystemError || size == 0) {
    setError(error, "An analysis worker input file has an invalid size.");
    return std::nullopt;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input) {
      setError(error, "Could not read an analysis worker input file.");
      return std::nullopt;
    }
    return document;
  } catch (const nlohmann::json::exception &) {
    setError(error, "An analysis worker input file is not valid JSON.");
    return std::nullopt;
  }
}

bool writeSpeechTranscriptionRequestFiles(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error) {
  if (request.model.empty() || !request.samples || request.sampleCount == 0 ||
      request.sampleCount > kMaximumSpeechSamples) {
    setError(error, "The analysis worker received an invalid speech request.");
    return false;
  }
  const std::filesystem::path audioPath = workspace / kAudioName;
  const std::filesystem::path audioStaging = stagingPath(audioPath);
  std::ofstream output(audioStaging, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create the speech staging file.");
    return false;
  }
  output.write(
      reinterpret_cast<const char *>(request.samples),
      static_cast<std::streamsize>(request.sampleCount * sizeof(float)));
  output.flush();
  output.close();
  if (!output) {
    std::error_code ignored;
    std::filesystem::remove(audioStaging, ignored);
    setError(error, "Could not finish the speech staging file.");
    return false;
  }
  if (!publish(audioStaging, audioPath, error))
    return false;
  return writeJson(
      workspace / kRequestName,
      {{"operation", "transcribe_speech"},
       {"model", pathUtf8(request.model)},
       {"alignment_preset", static_cast<int>(request.alignmentPreset)},
       {"source_language", request.sourceLanguage},
       {"task", static_cast<int>(request.task)},
       {"sample_count", request.sampleCount}},
      error);
}

bool loadRequest(const std::filesystem::path &workspace, OwnedRequest *owned,
                 std::string *error) {
  if (!owned) {
    setError(error, "The analysis worker request destination is missing.");
    return false;
  }
  const auto document = readJson(workspace / kRequestName, error);
  if (!document || !document->is_object()) {
    setError(error, "The analysis worker request is invalid.");
    return false;
  }
  OwnedRequest decoded;
  const std::string operation = document->value("operation", std::string{});
  if (operation == "review_video") {
    try {
      decoded.operation = OwnedRequest::Operation::ReviewVideo;
      auto &request = decoded.reviewRequest;
      request.sourcePath =
          pathFromUtf8(document->at("video_file").get<std::string>());
      request.videoStreamIndex = document->at("video_stream_index").get<int>();
      request.durationUs = document->at("duration_us").get<int64_t>();
      request.model = pathFromUtf8(document->at("model").get<std::string>());
      request.projector =
          pathFromUtf8(document->at("projector").get<std::string>());
      request.identity = document->at("identity").get<std::string>();
      for (const auto &cue : document->at("speech"))
        request.speech.push_back({cue.at("start_us").get<int64_t>(),
                                  cue.at("end_us").get<int64_t>(),
                                  cue.at("text").get<std::string>()});
      if (playback_video_analysis::reviewWindows(request.durationUs).empty() ||
          playback_video_analysis::reviewCachePath(request).empty() ||
          request.identity != playback_video_analysis::reviewIdentity(request))
        throw std::runtime_error("Invalid source identity");
    } catch (const std::exception &) {
      setError(
          error,
          "The video review worker request is invalid or the source changed.");
      return false;
    }
    *owned = std::move(decoded);
    return true;
  }
  if (operation == "transcribe_speech") {
    decoded.operation = OwnedRequest::Operation::TranscribeSpeech;
    try {
      const int alignment = document->at("alignment_preset").get<int>();
      const int task = document->at("task").get<int>();
      const std::size_t sampleCount =
          document->at("sample_count").get<std::size_t>();
      if (alignment <
              static_cast<int>(
                  playback_video_transcript::WhisperAlignmentPreset::None) ||
          alignment >
              static_cast<int>(playback_video_transcript::
                                   WhisperAlignmentPreset::LargeV3Turbo) ||
          task < static_cast<int>(
                     playback_video_transcript::WhisperTask::Transcribe) ||
          task >
              static_cast<int>(
                  playback_video_transcript::WhisperTask::TranslateToEnglish) ||
          sampleCount == 0 || sampleCount > kMaximumSpeechSamples) {
        setError(error, "The analysis worker speech request is invalid.");
        return false;
      }
      decoded.speechSamples.resize(sampleCount);
      decoded.speechRequest.model =
          pathFromUtf8(document->at("model").get<std::string>());
      decoded.speechRequest.alignmentPreset =
          static_cast<playback_video_transcript::WhisperAlignmentPreset>(
              alignment);
      decoded.speechRequest.sourceLanguage =
          document->at("source_language").get<std::string>();
      decoded.speechRequest.task =
          static_cast<playback_video_transcript::WhisperTask>(task);
      decoded.speechRequest.samples = decoded.speechSamples.data();
      decoded.speechRequest.sampleCount = decoded.speechSamples.size();
      std::ifstream audio(workspace / kAudioName, std::ios::binary);
      audio.read(reinterpret_cast<char *>(decoded.speechSamples.data()),
                 static_cast<std::streamsize>(decoded.speechSamples.size() *
                                              sizeof(float)));
      if (!audio || audio.peek() != std::ifstream::traits_type::eof()) {
        setError(error, "The analysis worker speech data is truncated.");
        return false;
      }
    } catch (const nlohmann::json::exception &) {
      setError(error, "The analysis worker speech document is invalid.");
      return false;
    } catch (const std::bad_alloc &) {
      setError(error, "The analysis worker speech request exceeds its bounds.");
      return false;
    }
    *owned = std::move(decoded);
    owned->speechRequest.samples = owned->speechSamples.data();
    return true;
  }
  setError(error, "Unknown analysis worker operation.");
  return false;
}

bool storeSpeechTranscriptionResult(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerResult &result,
    std::string *error) {
  nlohmann::json segments = nlohmann::json::array();
  for (const playback_video_transcript::RecognizedSegment &segment :
       result.document.segments) {
    nlohmann::json tokens = nlohmann::json::array();
    for (const playback_video_transcript::RecognizedToken &token :
         segment.tokens) {
      tokens.push_back({{"start_us", token.startUs},
                        {"end_us", token.endUs},
                        {"alignment_us", token.alignmentUs},
                        {"text", token.text}});
    }
    segments.push_back({{"start_us", segment.startUs},
                        {"end_us", segment.endUs},
                        {"text", segment.text},
                        {"tokens", std::move(tokens)}});
  }
  return writeJson(workspace / kResultName,
                   {{"operation", "transcribe_speech"},
                    {"status", static_cast<int>(result.status)},
                    {"detail", result.detail},
                    {"source_language", result.document.sourceLanguage},
                    {"segments", std::move(segments)}},
                   error);
}

class ProgressPublisher final {
public:
  explicit ProgressPublisher(std::filesystem::path workspace)
      : path_(std::move(workspace) / kProgressName) {}

  void publish(std::optional<double> progress, const std::string &phase) {
    const auto now = std::chrono::steady_clock::now();
    const bool phaseChanged = phase != phase_;
    if (!phaseChanged && lastWrite_.time_since_epoch().count() != 0 &&
        now - lastWrite_ < std::chrono::milliseconds(200)) {
      return;
    }
    phase_ = phase;
    lastWrite_ = now;
    ++sequence_;
    nlohmann::json fraction = nullptr;
    if (progress)
      fraction = std::clamp(*progress, 0.0, 1.0);
    std::string ignored;
    writeJson(path_,
              {{"sequence", sequence_},
               {"progress", std::move(fraction)},
               {"phase", phase}},
              &ignored);
  }

private:
  std::filesystem::path path_;
  std::chrono::steady_clock::time_point lastWrite_{};
  std::uint64_t sequence_ = 0;
  std::string phase_;
};

} // namespace

bool inference_worker_protocol::storeSpeechTranscriptionRequest(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error) {
  return writeSpeechTranscriptionRequestFiles(workspace, request, error);
}

bool inference_worker_protocol::storeVideoReviewRequest(
    const std::filesystem::path &workspace,
    const playback_video_analysis::ReviewRequest &request, std::string *error) {
  nlohmann::json speech = nlohmann::json::array();
  for (const auto &cue : request.speech)
    speech.push_back(
        {{"start_us", cue.startUs}, {"end_us", cue.endUs}, {"text", cue.text}});
  return writeJson(workspace / kRequestName,
                   {{"operation", "review_video"},
                    {"video_file", pathUtf8(request.sourcePath)},
                    {"video_stream_index", request.videoStreamIndex},
                    {"duration_us", request.durationUs},
                    {"model", pathUtf8(request.model)},
                    {"projector", pathUtf8(request.projector)},
                    {"identity", request.identity},
                    {"speech", std::move(speech)}},
                   error);
}

int inferenceWorkerMain(const std::filesystem::path &workspace) {
  std::string error;
  OwnedRequest owned;
  if (!loadRequest(workspace, &owned, &error)) {
    return 2;
  }
  ProgressPublisher progress(workspace);
  OperationControl control;
  control.cancelled = [] { return false; };
  control.backgroundGpuAllowed = [] { return true; };
  control.progress = [&](std::optional<double> fraction, std::string phase) {
    progress.publish(fraction, phase);
  };
  InferenceEngine engine;
  if (owned.operation == OwnedRequest::Operation::ReviewVideo) {
    using namespace playback_video_analysis;
    ReviewProgress completed;
    (void)loadReviewProgress(owned.reviewRequest, &completed);
    const auto result = engine.reviewVideo(
        owned.reviewRequest, control, completed,
        [&](const ReviewProgress &progress, std::string *detail) {
          if (owned.reviewRequest.identity !=
              reviewIdentity(owned.reviewRequest)) {
            *detail = "The source changed during video review.";
            return false;
          }
          return storeReviewProgress(owned.reviewRequest, progress, detail);
        });
    if (!writeJson(workspace / kResultName,
                   {{"operation", "review_video"},
                    {"status", static_cast<int>(result.status)},
                    {"detail", result.detail}},
                   &error))
      return 3;
    return result.status == OperationStatus::Succeeded ? 0 : 4;
  }
  if (owned.operation == OwnedRequest::Operation::TranscribeSpeech) {
    playback_video_transcript::SpeechWorkerResult result;
    playback_video_transcript::WhisperEngine whisper;
    std::string device;
    progress.publish(0.0, "Loading Vulkan speech model");
    if (!whisper.initialize(
            owned.speechRequest.model, owned.speechRequest.alignmentPreset,
            owned.speechRequest.task, owned.speechRequest.sourceLanguage,
            &device, &result.detail)) {
      result.status = playback_video_transcript::SpeechWorkerStatus::Failed;
    } else {
      progress.publish(0.02, "Vulkan ready on " + device);
      const bool succeeded = whisper.transcribe(
          owned.speechRequest.samples, owned.speechRequest.sampleCount,
          [&](int value) {
            progress.publish(
                std::clamp(static_cast<double>(value) / 100.0, 0.0, 1.0),
                owned.speechRequest.task == playback_video_transcript::
                                                WhisperTask::TranslateToEnglish
                    ? "Translating speech to English"
                    : "Transcribing audio");
          },
          [] { return false; }, &result.document.segments, &result.detail);
      result.status =
          succeeded ? playback_video_transcript::SpeechWorkerStatus::Succeeded
                    : playback_video_transcript::SpeechWorkerStatus::Failed;
      result.document.sourceLanguage = whisper.sourceLanguage();
    }
    if (!storeSpeechTranscriptionResult(workspace, result, &error))
      return 3;
    return result.status ==
                   playback_video_transcript::SpeechWorkerStatus::Succeeded
               ? 0
               : 4;
  }

  return 2;
}

} // namespace playback_video_analysis
