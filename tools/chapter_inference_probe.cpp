#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/evidence_plan.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/inference.h"
#include "playback/video/chapter/inference_worker.h"
#include "playback/video/chapter/inference_worker_protocol.h"
#include "playback/video/chapter/sampled_evidence.h"
#include "playback/video/transcript/document.h"
#include "playback/video/image_wic.h"

namespace {

struct OwnedFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
  std::int64_t timeUs = 0;
};

bool rgbaToRgb(const playback_video_image::RgbaImage &source,
               OwnedFrame *destination) {
  if (!destination || !playback_video_image::validate(source))
    return false;
  const std::size_t pixels =
      static_cast<std::size_t>(source.width) * source.height;
  if (pixels > (std::numeric_limits<std::size_t>::max)() / 3u)
    return false;
  destination->width = source.width;
  destination->height = source.height;
  destination->rgb.resize(pixels * 3u);
  for (std::uint32_t y = 0; y < source.height; ++y) {
    const std::uint8_t *input =
        source.pixels.data() + static_cast<std::size_t>(y) * source.strideBytes;
    std::uint8_t *output = destination->rgb.data() +
                           static_cast<std::size_t>(y) * source.width * 3u;
    for (std::uint32_t x = 0; x < source.width; ++x) {
      output[x * 3u + 0u] = input[x * 4u + 0u];
      output[x * 3u + 1u] = input[x * 4u + 1u];
      output[x * 3u + 2u] = input[x * 4u + 2u];
    }
  }
  return true;
}

bool loadEvidenceCache(const std::filesystem::path &path,
                       const playback_video_chapters::InferenceRequest &request,
                       playback_video_chapters::InferenceCheckpoint *checkpoint,
                       std::string *detail) {
  if (!checkpoint || path.empty())
    return true;
  std::error_code filesystemError;
  const bool exists = std::filesystem::exists(path, filesystemError);
  if (filesystemError) {
    if (detail)
      *detail = filesystemError.message();
    return false;
  }
  if (!exists)
    return true;
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    if (detail) {
      *detail = filesystemError ? filesystemError.message()
                                : "evidence cache is not a regular file";
    }
    return false;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw std::runtime_error("could not open evidence cache");
    nlohmann::json document;
    input >> document;
    if (!document.is_object() || !document.contains("observations")) {
      throw std::runtime_error("evidence cache has invalid fields");
    }
    std::vector<std::int64_t> sampleTimes;
    sampleTimes.reserve(request.frames.size());
    for (const auto &frame : request.frames)
      sampleTimes.push_back(frame.timeUs);
    const bool visualIdentityMatches =
        document.value("model", std::string{}) ==
            request.model.generic_u8string() &&
        document.value("projector", std::string{}) ==
            request.projector.generic_u8string() &&
        document.value("duration_us", std::int64_t{0}) == request.durationUs &&
        document.value("sample_times_us", std::vector<std::int64_t>{}) ==
            sampleTimes;
    nlohmann::json chapterPlan = nlohmann::json::array();
    for (const auto &chapter : request.chapterPlan) {
      chapterPlan.push_back(
          {{"start_us", chapter.startUs}, {"title", chapter.title}});
    }
    const bool current =
        document.value("schema", 0) ==
            playback_video_chapters::inference_worker_protocol::kSchema &&
        document.size() == 9 && visualIdentityMatches &&
        document.value("planner_model", std::string{}) ==
            request.plannerModel.generic_u8string() &&
        document.value("chapter_plan_adapter", std::string{}) ==
            request.chapterPlanAdapter.generic_u8string() &&
        document.value("chapter_plan", nlohmann::json{}) == chapterPlan;
    if (!current) {
      throw std::runtime_error("evidence cache does not match this request");
    }
    *checkpoint = {};
    checkpoint->model = request.model;
    checkpoint->projector = request.projector;
    checkpoint->plannerModel = request.plannerModel;
    checkpoint->chapterPlanAdapter = request.chapterPlanAdapter;
    checkpoint->durationUs = request.durationUs;
    checkpoint->chapterPlan = request.chapterPlan;
    checkpoint->observations =
        document["observations"].get<std::vector<std::string>>();
    checkpoint->sampleTimesUs = std::move(sampleTimes);
    return true;
  } catch (const std::exception &error) {
    if (detail)
      *detail = error.what();
    return false;
  }
}

bool storeEvidenceCache(
    const std::filesystem::path &path,
    const playback_video_chapters::InferenceCheckpoint &checkpoint,
    std::string *detail) {
  if (path.empty())
    return true;
  try {
    nlohmann::json chapterPlan = nlohmann::json::array();
    for (const auto &chapter : checkpoint.chapterPlan) {
      chapterPlan.push_back(
          {{"start_us", chapter.startUs}, {"title", chapter.title}});
    }
    nlohmann::json document = {
        {"schema",
         playback_video_chapters::inference_worker_protocol::kSchema},
        {"model", checkpoint.model.generic_u8string()},
        {"projector", checkpoint.projector.generic_u8string()},
        {"planner_model", checkpoint.plannerModel.generic_u8string()},
        {"chapter_plan_adapter",
         checkpoint.chapterPlanAdapter.generic_u8string()},
        {"chapter_plan", std::move(chapterPlan)},
        {"duration_us", checkpoint.durationUs},
        {"sample_times_us", checkpoint.sampleTimesUs},
        {"observations", checkpoint.observations}};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("could not create evidence cache");
    output << document.dump();
    output.flush();
    if (!output)
      throw std::runtime_error("could not write evidence cache");
    return true;
  } catch (const std::exception &error) {
    if (detail)
      *detail = error.what();
    return false;
  }
}

} // namespace

int main(int argc, char **argv) {
  using namespace playback_video_chapters;
  const bool exerciseYield =
      argc == 10 && std::string_view(argv[9]) == "--exercise-yield";
  const bool useEvidenceCache =
      argc == 11 && std::string_view(argv[9]) == "--evidence-cache";
  if (argc != 9 && !exerciseYield && !useEvidenceCache) {
    std::cerr << "usage: chapter_inference_probe <model.gguf> "
                  "<projector.gguf> <planner-model.gguf> "
                  "<speech-plan-adapter.gguf> <chapter-plan-adapter.gguf> "
                  "<png-directory> <english.srt> "
                 "<duration-us> "
                 "[--exercise-yield | --evidence-cache <cache.json>]\n";
    return 2;
  }

  std::int64_t durationUs = 0;
  try {
    durationUs = std::stoll(argv[8]);
  } catch (const std::exception &) {
    std::cerr << "invalid duration\n";
    return 2;
  }
  const std::filesystem::path input = argv[6];
  std::vector<OwnedFrame> frames;
  std::error_code error;
  if (std::filesystem::is_directory(input, error) && !error) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(input, error)) {
      if (error)
        break;
      if (entry.is_regular_file() && entry.path().extension() == ".png") {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    if (error || files.size() < kMinimumAutomaticChapterCount ||
        files.size() > kMaximumAutomaticChapterCount) {
      std::cerr << "expected " << kMinimumAutomaticChapterCount
                << " through " << kMaximumAutomaticChapterCount
                << " chronological PNG candidate frames, found " << files.size()
                << '\n';
      return 2;
    }

    playback_video_image::WicCodec codec;
    std::string detail;
    if (!codec.open(&detail)) {
      std::cerr << detail << '\n';
      return 2;
    }
    frames.resize(files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
      playback_video_image::RgbaImage decoded;
      if (!codec.decodeFile(files[index], &decoded, {}, &detail) ||
          !rgbaToRgb(decoded, &frames[index])) {
        std::cerr << "could not decode " << files[index] << ": " << detail
                  << '\n';
        return 2;
      }
    }
  } else {
    std::cerr << "input is not a chronological PNG directory; direct video "
                 "sampling requires the production ASR planner\n";
    return 2;
  }

  InferenceRequest request;
  request.model = argv[1];
  request.projector = argv[2];
  request.plannerModel = argv[3];
  request.chapterPlanAdapter = argv[5];
  request.durationUs = durationUs;
  std::vector<playback_video_transcript::Segment> transcript;
  std::string transcriptError;
  if (!playback_video_transcript::readIndexedTranscript(
          std::filesystem::path(argv[7]), &transcript, &transcriptError)) {
    std::cerr << "could not read English transcript: " << transcriptError
              << '\n';
    return 2;
  }
  request.englishDialogue.reserve(transcript.size());
  for (const playback_video_transcript::Segment &cue : transcript) {
    if (cue.startUs >= 0 && cue.startUs < durationUs && !cue.text.empty())
      request.englishDialogue.push_back({cue.startUs, cue.text});
  }
  if (request.englishDialogue.empty()) {
    std::cerr << "English transcript has no cues inside the video timeline\n";
    return 2;
  }
  std::string lastPhase;
  int lastPercentage = -2;
  std::atomic<bool> gpuAllowed{true};
  std::atomic<bool> yieldTriggered{false};
  OperationControl control;
  control.cancelled = [] { return false; };
  control.backgroundGpuAllowed = [&] {
    return gpuAllowed.load(std::memory_order_acquire);
  };
  control.progress = [&](std::optional<double> progress, std::string phase) {
    const int percentage = progress ? static_cast<int>(*progress * 100.0) : -1;
    if (phase == lastPhase && percentage == lastPercentage)
      return;
    lastPhase = phase;
    lastPercentage = percentage;
    std::cerr << percentage << "% " << phase << '\n';
    if (exerciseYield && progress && *progress >= 0.78 &&
        !yieldTriggered.exchange(true, std::memory_order_acq_rel)) {
      gpuAllowed.store(false, std::memory_order_release);
    }
  };

  SpeechChapterPlanRequest planRequest;
  planRequest.plannerModel = request.plannerModel;
  planRequest.planAdapter = argv[4];
  planRequest.durationUs = durationUs;
  planRequest.englishDialogue = request.englishDialogue;
  InferenceEngine planEngine;
  SpeechChapterPlanResult plan =
      planEngine.planChaptersFromSpeech(planRequest, control);
  if (plan.status != OperationStatus::Succeeded) {
    std::cerr << "speech plan failed (" << static_cast<int>(plan.status)
              << "): " << plan.detail << '\n';
    return 1;
  }
  if (plan.chapterPlan.size() != frames.size()) {
    std::cerr << "expected one chronological PNG for each of the "
              << plan.chapterPlan.size() << " ASR-planned chapters, found "
              << frames.size() << '\n';
    return 2;
  }
  request.chapterPlan = std::move(plan.chapterPlan);
  std::vector<std::int64_t> chapterStartsUs;
  chapterStartsUs.reserve(request.chapterPlan.size());
  for (const GeneratedChapterPlanEntry &chapter : request.chapterPlan)
    chapterStartsUs.push_back(chapter.startUs);
  const std::vector<std::int64_t> sampleTimesUs =
      buildSpeechGuidedFrameSchedule(request.durationUs, chapterStartsUs);
  if (sampleTimesUs.size() != frames.size()) {
    std::cerr << "speech plan returned an invalid frame schedule\n";
    return 2;
  }
  request.frames.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    OwnedFrame &frame = frames[index];
    frame.timeUs = sampleTimesUs[index];
    request.frames.push_back(
        {frame.width, frame.height, &frame.rgb, frame.timeUs});
  }

  InferenceCheckpoint checkpoint;
  const std::filesystem::path evidenceCache =
      useEvidenceCache ? std::filesystem::path(argv[10])
                       : std::filesystem::path{};
  std::string cacheDetail;
  if (!exerciseYield &&
      !loadEvidenceCache(evidenceCache, request, &checkpoint, &cacheDetail)) {
    std::cerr << "could not load probe evidence cache: " << cacheDetail << '\n';
    return 2;
  }
  InferenceResult inference;
  if (exerciseYield) {
    const std::string workerKey(64, 'e');
    discardInferenceWorkerWorkspace(workerKey);
    InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(workerKey, control);
    if (acquired.status != OperationStatus::Succeeded) {
      std::cerr << "could not acquire the isolated inference workspace: "
                << acquired.detail << '\n';
      return 1;
    }
    inference = runInferenceWorker(request, control, acquired.lease);
    if (inference.status != OperationStatus::Yielded) {
      std::cerr << "the isolated inference worker did not yield to foreground "
                   "GPU ownership (status "
                << static_cast<int>(inference.status)
                << "): " << inference.detail << '\n';
      discardInferenceWorkerWorkspace(acquired.lease);
      return 1;
    }
    acquired.lease = {};
    std::cerr << "YIELDED after terminating the isolated Vulkan worker\n";
    gpuAllowed.store(true, std::memory_order_release);
    lastPhase.clear();
    lastPercentage = -2;
    acquired = acquireInferenceWorkspaceLease(workerKey, control);
    if (acquired.status != OperationStatus::Succeeded) {
      std::cerr << "could not reacquire the isolated inference workspace: "
                << acquired.detail << '\n';
      return 1;
    }
    inference = runInferenceWorker(request, control, acquired.lease);
    if (inference.status == OperationStatus::Succeeded) {
      discardInferenceWorkerWorkspace(acquired.lease);
    }
  } else {
    InferenceEngine engine;
    inference = engine.run(request, control, &checkpoint);
  }
  if (!exerciseYield &&
      !storeEvidenceCache(evidenceCache, checkpoint, &cacheDetail)) {
    std::cerr << "could not store probe evidence cache: " << cacheDetail
              << '\n';
    return 2;
  }
  if (inference.status != OperationStatus::Succeeded) {
    std::cerr << "inference failed (" << static_cast<int>(inference.status)
              << "): " << inference.detail << '\n';
    return 1;
  }
  const auto emitEvidence = [&](std::ostream &output) {
    for (std::size_t index = 0;
         index < frames.size() && index < checkpoint.observations.size();
         ++index) {
      output << "EVIDENCE\t" << index + 1 << '\t' << frames[index].timeUs
             << '\t' << checkpoint.observations[index] << '\n';
    }
  };
  AnalysisResult result =
      materializeGeneratedDocument(inference.document, durationUs);
  if (result.status != OperationStatus::Succeeded) {
    emitEvidence(std::cerr);
    std::cerr << "BOUNDARIES";
    for (const GeneratedChapter &chapter : inference.document.chapters) {
      std::cerr << '\t' << chapter.startUs;
    }
    std::cerr << '\n';
    for (const GeneratedChapter &chapter : inference.document.chapters) {
      std::cerr << "GENERATED_CHAPTER\t" << chapter.startUs << '\t'
                << chapter.title << '\n';
    }
    std::cerr << "artifact rejected: " << result.detail << '\n';
    return 1;
  }

  emitEvidence(std::cout);
  std::cout << "BOUNDARIES";
  for (const GeneratedChapter &chapter : inference.document.chapters) {
    std::cout << '\t' << chapter.startUs;
  }
  std::cout << '\n';
  for (const Chapter &chapter : result.chapters) {
    std::cout << "CHAPTER\t" << chapter.startUs << '\t' << chapter.endUs << '\t'
              << chapter.title << '\n';
  }
  return 0;
}
