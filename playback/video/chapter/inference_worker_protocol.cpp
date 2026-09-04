#include "playback/video/chapter/inference_worker_protocol.h"
#include "playback/video/chapter/inference_worker.h"

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

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {
namespace {

constexpr std::uintmax_t kMaximumManifestBytes = 16u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumCheckpointBytes = 4u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumFrameBytes = 1024ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumSpeechSamples =
    60ull * playback_video_transcript::WhisperEngine::kSampleRate;
constexpr std::size_t kMaximumDialogueCues = 200'000;

std::atomic<std::uint64_t> gStagingSequence{0};

const std::filesystem::path kRequestName = L"request.json";
const std::filesystem::path kFramesName = L"frames.rgb";
const std::filesystem::path kAudioName = L"audio.f32";
const std::filesystem::path kCheckpointName = L"checkpoint.json";
const std::filesystem::path kProgressName = L"progress.json";
const std::filesystem::path kResultName = L"result.json";

struct OwnedRequest {
  enum class Operation {
    Analyze,
    PlanChaptersFromSpeech,
    TranscribeSpeech,
  } operation = Operation::Analyze;
  InferenceRequest request;
  SpeechChapterPlanRequest planRequest;
  playback_video_transcript::SpeechWorkerRequest speechRequest;
  std::vector<float> speechSamples;
  std::vector<std::vector<std::uint8_t>> pixels;
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
  setError(error, "Could not atomically publish a chapter worker file "
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
    setError(error, "Could not create a chapter worker staging file.");
    return false;
  }
  output << document.dump() << '\n';
  output.flush();
  if (!output) {
    output.close();
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    setError(error, "Could not finish a chapter worker staging file.");
    return false;
  }
  output.close();
  return publish(staging, path, error);
}

std::optional<nlohmann::json> readJson(const std::filesystem::path &path,
                                       std::uintmax_t maximumBytes,
                                       std::string *error) {
  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    setError(error, "A chapter worker input file is missing.");
    return std::nullopt;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
  if (filesystemError || size == 0 || size > maximumBytes) {
    setError(error, "A chapter worker input file has an invalid size.");
    return std::nullopt;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input) {
      setError(error, "Could not read a chapter worker input file.");
      return std::nullopt;
    }
    return document;
  } catch (const nlohmann::json::exception &) {
    setError(error, "A chapter worker input file is not valid JSON.");
    return std::nullopt;
  }
}

bool validRgbSize(std::uint32_t width, std::uint32_t height,
                  std::size_t bytes) {
  return width > 0 && height > 0 &&
         width <= (std::numeric_limits<std::size_t>::max)() / height / 3u &&
         bytes == static_cast<std::size_t>(width) * height * 3u;
}

bool writeRequestFiles(const std::filesystem::path &workspace,
                       const InferenceRequest &request, std::string *error) {
  std::error_code ignoredAudio;
  std::filesystem::remove(workspace / kAudioName, ignoredAudio);
  nlohmann::json windows = nlohmann::json::array();
  std::uintmax_t totalBytes = 0;
  for (const InferenceTemporalWindow &window : request.windows) {
    nlohmann::json frames = nlohmann::json::array();
    for (const InferenceFrame &frame : window.frames) {
      if (!frame.imageRgb ||
          !validRgbSize(frame.imageWidth, frame.imageHeight,
                        frame.imageRgb->size()) ||
          totalBytes > kMaximumFrameBytes - frame.imageRgb->size()) {
        setError(error, "The chapter worker received invalid frame data.");
        return false;
      }
      totalBytes += frame.imageRgb->size();
      frames.push_back({{"time_us", frame.timeUs},
                        {"width", frame.imageWidth},
                        {"height", frame.imageHeight},
                        {"bytes", frame.imageRgb->size()}});
    }
    windows.push_back({{"start_us", window.intervalStartUs},
                       {"end_us", window.intervalEndUs},
                       {"frames", std::move(frames)}});
  }
  if (totalBytes == 0 || totalBytes > kMaximumFrameBytes ||
      request.englishDialogue.size() > kMaximumDialogueCues ||
      request.chapterPlan.size() != request.windows.size() ||
      request.chapterPlan.size() < kMinimumAutomaticChapterCount ||
      request.chapterPlan.size() > kMaximumAutomaticChapterCount) {
    setError(error, "The chapter worker request exceeds its storage bounds.");
    return false;
  }

  const std::filesystem::path framesPath = workspace / kFramesName;
  const std::filesystem::path framesStaging = stagingPath(framesPath);
  std::ofstream frameOutput(framesStaging, std::ios::binary | std::ios::trunc);
  if (!frameOutput) {
    setError(error, "Could not create the chapter frame staging file.");
    return false;
  }
  for (const InferenceTemporalWindow &window : request.windows) {
    for (const InferenceFrame &frame : window.frames) {
      frameOutput.write(reinterpret_cast<const char *>(frame.imageRgb->data()),
                        static_cast<std::streamsize>(frame.imageRgb->size()));
      if (!frameOutput) {
        frameOutput.close();
        std::error_code ignored;
        std::filesystem::remove(framesStaging, ignored);
        setError(error, "Could not finish the chapter frame staging file.");
        return false;
      }
    }
  }
  frameOutput.flush();
  frameOutput.close();
  if (!publish(framesStaging, framesPath, error))
    return false;

  nlohmann::json dialogue = nlohmann::json::array();
  for (const InferenceDialogueCue &cue : request.englishDialogue) {
    dialogue.push_back({{"time_us", cue.timeUs}, {"text", cue.text}});
  }
  nlohmann::json chapterPlan = nlohmann::json::array();
  for (const GeneratedChapterPlanEntry &chapter : request.chapterPlan) {
    chapterPlan.push_back(
        {{"start_us", chapter.startUs}, {"title", chapter.title}});
  }
  const nlohmann::json manifest = {
      {"schema", inference_worker_protocol::kSchema},
      {"operation", "analyze"},
      {"duration_us", request.durationUs},
      {"model", pathUtf8(request.model)},
      {"projector", pathUtf8(request.projector)},
      {"planner_model", pathUtf8(request.plannerModel)},
      {"chapter_plan_adapter", pathUtf8(request.chapterPlanAdapter)},
      {"frame_bytes", totalBytes},
      {"windows", std::move(windows)},
      {"english_dialogue", std::move(dialogue)},
      {"chapter_plan", std::move(chapterPlan)}};
  return writeJson(workspace / kRequestName, manifest, error);
}

bool writeSpeechChapterPlanRequestFiles(
    const std::filesystem::path &workspace,
    const SpeechChapterPlanRequest &request, std::string *error) {
  if (request.plannerModel.empty() || request.planAdapter.empty() ||
      request.durationUs <= 0 || request.englishDialogue.empty() ||
      request.englishDialogue.size() > kMaximumDialogueCues) {
    setError(error,
             "The chapter worker received an invalid speech-plan request.");
    return false;
  }
  nlohmann::json dialogue = nlohmann::json::array();
  for (const InferenceDialogueCue &cue : request.englishDialogue) {
    dialogue.push_back({{"time_us", cue.timeUs}, {"text", cue.text}});
  }
  std::error_code ignored;
  std::filesystem::remove(workspace / kFramesName, ignored);
  std::filesystem::remove(workspace / kAudioName, ignored);
  return writeJson(workspace / kRequestName,
                   {{"schema", inference_worker_protocol::kSchema},
                    {"operation", "plan_chapters_from_speech"},
                    {"duration_us", request.durationUs},
                    {"planner_model", pathUtf8(request.plannerModel)},
                    {"plan_adapter", pathUtf8(request.planAdapter)},
                    {"english_dialogue", std::move(dialogue)}},
                   error);
}

bool writeSpeechTranscriptionRequestFiles(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error) {
  if (request.model.empty() || !request.samples || request.sampleCount == 0 ||
      request.sampleCount > kMaximumSpeechSamples) {
    setError(error,
             "The chapter worker received an invalid speech request.");
    return false;
  }
  const std::filesystem::path audioPath = workspace / kAudioName;
  const std::filesystem::path audioStaging = stagingPath(audioPath);
  std::ofstream output(audioStaging, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create the speech staging file.");
    return false;
  }
  output.write(reinterpret_cast<const char *>(request.samples),
               static_cast<std::streamsize>(request.sampleCount *
                                            sizeof(float)));
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
  std::error_code ignored;
  std::filesystem::remove(workspace / kFramesName, ignored);
  return writeJson(
      workspace / kRequestName,
      {{"schema", inference_worker_protocol::kSchema},
       {"operation", "transcribe_speech"},
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
    setError(error, "The chapter worker request destination is missing.");
    return false;
  }
  const auto manifest =
      readJson(workspace / kRequestName, kMaximumManifestBytes, error);
  if (!manifest || !manifest->is_object() ||
      manifest->value("schema", 0) != inference_worker_protocol::kSchema) {
    setError(error, "The chapter worker request manifest is invalid.");
    return false;
  }
  OwnedRequest decoded;
  const std::string operation = manifest->value("operation", std::string{});
  if (operation == "transcribe_speech") {
    decoded.operation = OwnedRequest::Operation::TranscribeSpeech;
    try {
      const int alignment = manifest->at("alignment_preset").get<int>();
      const int task = manifest->at("task").get<int>();
      const std::size_t sampleCount =
          manifest->at("sample_count").get<std::size_t>();
      if (alignment < static_cast<int>(
                          playback_video_transcript::WhisperAlignmentPreset::
                              None) ||
          alignment > static_cast<int>(
                          playback_video_transcript::WhisperAlignmentPreset::
                              LargeV3Turbo) ||
          task < static_cast<int>(
                     playback_video_transcript::WhisperTask::Transcribe) ||
          task > static_cast<int>(playback_video_transcript::WhisperTask::
                                      TranslateToEnglish) ||
          sampleCount == 0 || sampleCount > kMaximumSpeechSamples) {
        setError(error, "The chapter worker speech request is invalid.");
        return false;
      }
      decoded.speechSamples.resize(sampleCount);
      decoded.speechRequest.model =
          pathFromUtf8(manifest->at("model").get<std::string>());
      decoded.speechRequest.alignmentPreset =
          static_cast<playback_video_transcript::WhisperAlignmentPreset>(
              alignment);
      decoded.speechRequest.sourceLanguage =
          manifest->at("source_language").get<std::string>();
      decoded.speechRequest.task =
          static_cast<playback_video_transcript::WhisperTask>(task);
      decoded.speechRequest.samples = decoded.speechSamples.data();
      decoded.speechRequest.sampleCount = decoded.speechSamples.size();
      std::ifstream audio(workspace / kAudioName, std::ios::binary);
      audio.read(reinterpret_cast<char *>(decoded.speechSamples.data()),
                 static_cast<std::streamsize>(decoded.speechSamples.size() *
                                              sizeof(float)));
      if (!audio || audio.peek() != std::ifstream::traits_type::eof()) {
        setError(error, "The chapter worker speech data is truncated.");
        return false;
      }
    } catch (const nlohmann::json::exception &) {
      setError(error, "The chapter worker speech manifest is invalid.");
      return false;
    } catch (const std::bad_alloc &) {
      setError(error, "The chapter worker speech request exceeds its bounds.");
      return false;
    }
    *owned = std::move(decoded);
    owned->speechRequest.samples = owned->speechSamples.data();
    return true;
  }
  if (!manifest->contains("english_dialogue") ||
      !(*manifest)["english_dialogue"].is_array()) {
    setError(error, "The chapter worker request has no dialogue timeline.");
    return false;
  }
  if (operation == "plan_chapters_from_speech") {
    decoded.operation = OwnedRequest::Operation::PlanChaptersFromSpeech;
    try {
      decoded.planRequest.durationUs =
          manifest->at("duration_us").get<std::int64_t>();
      decoded.planRequest.plannerModel =
          pathFromUtf8(manifest->at("planner_model").get<std::string>());
      decoded.planRequest.planAdapter =
          pathFromUtf8(manifest->at("plan_adapter").get<std::string>());
      for (const auto &cue : (*manifest)["english_dialogue"]) {
        if (decoded.planRequest.englishDialogue.size() >=
            kMaximumDialogueCues) {
          throw std::bad_alloc();
        }
        decoded.planRequest.englishDialogue.push_back(
            {cue.at("time_us").get<std::int64_t>(),
             cue.at("text").get<std::string>()});
      }
    } catch (const nlohmann::json::exception &) {
      setError(error, "The chapter worker speech-plan manifest is invalid.");
      return false;
    } catch (const std::bad_alloc &) {
      setError(error,
               "The chapter worker speech plan exceeds its memory bounds.");
      return false;
    }
    *owned = std::move(decoded);
    return true;
  }
  if (operation != "analyze" || !manifest->contains("windows") ||
      !(*manifest)["windows"].is_array() ||
      !manifest->contains("chapter_plan") ||
      !(*manifest)["chapter_plan"].is_array()) {
    setError(error, "The chapter worker operation is invalid.");
    return false;
  }
  try {
    decoded.request.durationUs =
        manifest->at("duration_us").get<std::int64_t>();
    decoded.request.model =
        pathFromUtf8(manifest->at("model").get<std::string>());
    decoded.request.projector =
        pathFromUtf8(manifest->at("projector").get<std::string>());
    decoded.request.plannerModel =
        pathFromUtf8(manifest->at("planner_model").get<std::string>());
    decoded.request.chapterPlanAdapter = pathFromUtf8(
        manifest->at("chapter_plan_adapter").get<std::string>());
  } catch (const nlohmann::json::exception &) {
    setError(error, "The chapter worker request header is invalid.");
    return false;
  }

  const auto &jsonWindows = (*manifest)["windows"];
  if (jsonWindows.size() < kMinimumAutomaticEvidenceSampleCount ||
      jsonWindows.size() > kMaximumAutomaticEvidenceSampleCount ||
      (*manifest)["chapter_plan"].size() != jsonWindows.size()) {
    setError(error, "The chapter worker request has an invalid timeline.");
    return false;
  }
  std::size_t totalFrames = 0;
  for (const auto &window : jsonWindows) {
    if (!window.is_object() || !window.contains("frames") ||
        !window["frames"].is_array() || window["frames"].size() != 1) {
      setError(error, "The chapter worker request has invalid frame groups.");
      return false;
    }
    totalFrames += window["frames"].size();
  }
  decoded.pixels.resize(totalFrames);

  std::ifstream frameInput(workspace / kFramesName, std::ios::binary);
  if (!frameInput) {
    setError(error, "The chapter worker frame data is missing.");
    return false;
  }
  std::size_t frameIndex = 0;
  std::uintmax_t totalBytes = 0;
  try {
    for (const auto &window : jsonWindows) {
      for (const auto &frame : window["frames"]) {
        const std::uint32_t width = frame.at("width").get<std::uint32_t>();
        const std::uint32_t height = frame.at("height").get<std::uint32_t>();
        const std::size_t bytes = frame.at("bytes").get<std::size_t>();
        if (!validRgbSize(width, height, bytes) ||
            totalBytes > kMaximumFrameBytes - bytes) {
          setError(error, "The chapter worker frame dimensions are invalid.");
          return false;
        }
        totalBytes += bytes;
        decoded.pixels[frameIndex].resize(bytes);
        frameInput.read(
            reinterpret_cast<char *>(decoded.pixels[frameIndex].data()),
            static_cast<std::streamsize>(bytes));
        if (!frameInput) {
          setError(error, "The chapter worker frame data is truncated.");
          return false;
        }
        ++frameIndex;
      }
    }
  } catch (const nlohmann::json::exception &) {
    setError(error, "The chapter worker frame manifest is invalid.");
    return false;
  } catch (const std::bad_alloc &) {
    setError(error, "The chapter worker could not allocate frame storage.");
    return false;
  }
  if (frameInput.peek() != std::ifstream::traits_type::eof() ||
      manifest->value("frame_bytes", std::uintmax_t{0}) != totalBytes) {
    setError(error, "The chapter worker frame data size does not match.");
    return false;
  }

  decoded.request.windows.reserve(jsonWindows.size());
  frameIndex = 0;
  try {
    for (const auto &jsonWindow : jsonWindows) {
      InferenceTemporalWindow window;
      window.intervalStartUs = jsonWindow.at("start_us").get<std::int64_t>();
      window.intervalEndUs = jsonWindow.at("end_us").get<std::int64_t>();
      window.frames.reserve(jsonWindow["frames"].size());
      for (const auto &jsonFrame : jsonWindow["frames"]) {
        window.frames.push_back({jsonFrame.at("width").get<std::uint32_t>(),
                                 jsonFrame.at("height").get<std::uint32_t>(),
                                 &decoded.pixels[frameIndex],
                                 jsonFrame.at("time_us").get<std::int64_t>()});
        ++frameIndex;
      }
      decoded.request.windows.push_back(std::move(window));
    }
    for (const auto &cue : (*manifest)["english_dialogue"]) {
      if (decoded.request.englishDialogue.size() >= kMaximumDialogueCues)
        throw std::bad_alloc();
      decoded.request.englishDialogue.push_back(
          {cue.at("time_us").get<std::int64_t>(),
           cue.at("text").get<std::string>()});
    }
    for (const auto &chapter : (*manifest)["chapter_plan"]) {
      decoded.request.chapterPlan.push_back(
          {chapter.at("start_us").get<std::int64_t>(),
           chapter.at("title").get<std::string>()});
    }
  } catch (const nlohmann::json::exception &) {
    setError(error, "The chapter worker timeline manifest is invalid.");
    return false;
  } catch (const std::bad_alloc &) {
    setError(error, "The chapter worker timeline exceeds its memory bounds.");
    return false;
  }
  *owned = std::move(decoded);
  return true;
}

nlohmann::json checkpointJson(const InferenceCheckpoint &checkpoint) {
  nlohmann::json plan = nlohmann::json::array();
  for (const GeneratedChapterPlanEntry &entry : checkpoint.chapterPlan) {
    plan.push_back({{"start_us", entry.startUs}, {"title", entry.title}});
  }
  return {{"schema", inference_worker_protocol::kSchema},
          {"duration_us", checkpoint.durationUs},
          {"sample_times_us", checkpoint.sampleTimesUs},
          {"interval_starts_us", checkpoint.intervalStartsUs},
          {"interval_ends_us", checkpoint.intervalEndsUs},
          {"observations", checkpoint.observations},
          {"chapter_plan", std::move(plan)}};
}

bool storeCheckpoint(const std::filesystem::path &workspace,
                     const InferenceCheckpoint &checkpoint,
                     std::string *error) {
  return writeJson(workspace / kCheckpointName, checkpointJson(checkpoint),
                   error);
}

bool loadCheckpoint(const std::filesystem::path &workspace,
                    const InferenceRequest &request,
                    InferenceCheckpoint *checkpoint) {
  std::error_code existsError;
  if (!std::filesystem::exists(workspace / kCheckpointName, existsError) ||
      existsError) {
    return false;
  }
  const auto document =
      readJson(workspace / kCheckpointName, kMaximumCheckpointBytes, nullptr);
  if (!document || !document->is_object() ||
      document->value("schema", 0) != inference_worker_protocol::kSchema)
    return false;
  InferenceCheckpoint decoded;
  decoded.model = request.model;
  decoded.projector = request.projector;
  decoded.plannerModel = request.plannerModel;
  decoded.chapterPlanAdapter = request.chapterPlanAdapter;
  try {
    decoded.durationUs = document->at("duration_us").get<std::int64_t>();
    decoded.sampleTimesUs = document->at("sample_times_us")
                                .get<std::vector<std::vector<std::int64_t>>>();
    decoded.intervalStartsUs =
        document->at("interval_starts_us").get<std::vector<std::int64_t>>();
    decoded.intervalEndsUs =
        document->at("interval_ends_us").get<std::vector<std::int64_t>>();
    decoded.observations =
        document->at("observations").get<std::vector<std::string>>();
    for (const auto &entry : document->at("chapter_plan")) {
      decoded.chapterPlan.push_back({entry.at("start_us").get<std::int64_t>(),
                                     entry.at("title").get<std::string>()});
    }
  } catch (const nlohmann::json::exception &) {
    return false;
  } catch (const std::bad_alloc &) {
    return false;
  }
  *checkpoint = std::move(decoded);
  return true;
}

bool storeResult(const std::filesystem::path &workspace,
                 const InferenceResult &result, std::string *error) {
  nlohmann::json chapters = nlohmann::json::array();
  for (const GeneratedChapter &chapter : result.document.chapters) {
    chapters.push_back({{"start_us", chapter.startUs},
                        {"title", chapter.title}});
  }
  return writeJson(workspace / kResultName,
                   {{"schema", inference_worker_protocol::kSchema},
                    {"operation", "analyze"},
                    {"status", static_cast<int>(result.status)},
                    {"detail", result.detail},
                    {"chapters", std::move(chapters)}},
                   error);
}

bool storeSpeechChapterPlanResult(const std::filesystem::path &workspace,
                                  const SpeechChapterPlanResult &result,
                                  std::string *error) {
  nlohmann::json plan = nlohmann::json::array();
  for (const GeneratedChapterPlanEntry &chapter : result.chapterPlan) {
    plan.push_back({{"start_us", chapter.startUs}, {"title", chapter.title}});
  }
  return writeJson(
      workspace / kResultName,
      {{"schema", inference_worker_protocol::kSchema},
       {"operation", "plan_chapters_from_speech"},
       {"status", static_cast<int>(result.status)},
       {"detail", result.detail},
       {"chapter_plan", std::move(plan)}},
      error);
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
  return writeJson(
      workspace / kResultName,
      {{"schema", inference_worker_protocol::kSchema},
       {"operation", "transcribe_speech"},
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
              {{"schema", inference_worker_protocol::kSchema},
               {"sequence", sequence_},
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

bool inference_worker_protocol::storeRequest(
    const std::filesystem::path &workspace, const InferenceRequest &request,
    std::string *error) {
  return writeRequestFiles(workspace, request, error);
}

bool inference_worker_protocol::storeSpeechChapterPlanRequest(
    const std::filesystem::path &workspace,
    const SpeechChapterPlanRequest &request, std::string *error) {
  return writeSpeechChapterPlanRequestFiles(workspace, request, error);
}

bool inference_worker_protocol::storeSpeechTranscriptionRequest(
    const std::filesystem::path &workspace,
    const playback_video_transcript::SpeechWorkerRequest &request,
    std::string *error) {
  return writeSpeechTranscriptionRequestFiles(workspace, request, error);
}

int inferenceWorkerMain(const std::filesystem::path &workspace) {
  std::string error;
  OwnedRequest owned;
  if (!loadRequest(workspace, &owned, &error)) {
    storeResult(workspace, {OperationStatus::Failed, std::move(error), {}},
                nullptr);
    return 2;
  }
  InferenceCheckpoint checkpoint;
  (void)loadCheckpoint(workspace, owned.request, &checkpoint);
  ProgressPublisher progress(workspace);
  OperationControl control;
  control.cancelled = [] { return false; };
  control.backgroundGpuAllowed = [] { return true; };
  control.progress = [&](std::optional<double> fraction, std::string phase) {
    progress.publish(fraction, phase);
  };
  InferenceEngine engine;
  if (owned.operation == OwnedRequest::Operation::TranscribeSpeech) {
    playback_video_transcript::SpeechWorkerResult result;
    playback_video_transcript::WhisperEngine whisper;
    std::string device;
    progress.publish(0.0, "Loading Vulkan speech model");
    if (!whisper.initialize(
            owned.speechRequest.model,
            owned.speechRequest.alignmentPreset,
            owned.speechRequest.sourceLanguage, &device, &result.detail)) {
      result.status = playback_video_transcript::SpeechWorkerStatus::Failed;
    } else {
      progress.publish(0.02, "Vulkan ready on " + device);
      const bool succeeded = whisper.transcribe(
          owned.speechRequest.samples, owned.speechRequest.sampleCount,
          [&](int value) {
            progress.publish(
                std::clamp(static_cast<double>(value) / 100.0, 0.0, 1.0),
                owned.speechRequest.task ==
                        playback_video_transcript::WhisperTask::
                            TranslateToEnglish
                    ? "Translating speech to English"
                    : "Transcribing audio");
          },
          [] { return false; }, &result.document.segments, &result.detail,
          owned.speechRequest.task);
      result.status = succeeded
                          ? playback_video_transcript::SpeechWorkerStatus::
                                Succeeded
                          : playback_video_transcript::SpeechWorkerStatus::
                                Failed;
      result.document.sourceLanguage = whisper.sourceLanguage();
    }
    if (!storeSpeechTranscriptionResult(workspace, result, &error))
      return 3;
    return result.status ==
                   playback_video_transcript::SpeechWorkerStatus::Succeeded
               ? 0
               : 4;
  }
  if (owned.operation == OwnedRequest::Operation::PlanChaptersFromSpeech) {
    const SpeechChapterPlanResult result =
        engine.planChaptersFromSpeech(owned.planRequest, control);
    if (!storeSpeechChapterPlanResult(workspace, result, &error))
      return 3;
    return result.status == OperationStatus::Succeeded ? 0 : 4;
  }
  const InferenceResult result = engine.run(
      owned.request, control, &checkpoint,
      [&](const InferenceCheckpoint &value, std::string *checkpointError) {
        return storeCheckpoint(workspace, value, checkpointError);
      });
  if (!storeResult(workspace, result, &error))
    return 3;
  return result.status == OperationStatus::Succeeded ? 0 : 4;
}

} // namespace playback_video_chapters
