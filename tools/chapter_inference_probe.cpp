#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/inference.h"
#include "playback/video/chapter/sampled_evidence.h"
#include "playback/video/image_wic.h"

namespace {

struct OwnedFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
};

bool rgbaToRgb(const playback_video_image::RgbaImage& source,
               OwnedFrame* destination) {
  if (!destination || !playback_video_image::validate(source)) return false;
  const std::size_t pixels =
      static_cast<std::size_t>(source.width) * source.height;
  if (pixels > (std::numeric_limits<std::size_t>::max)() / 3u) return false;
  destination->width = source.width;
  destination->height = source.height;
  destination->rgb.resize(pixels * 3u);
  for (std::uint32_t y = 0; y < source.height; ++y) {
    const std::uint8_t* input =
        source.pixels.data() + static_cast<std::size_t>(y) * source.strideBytes;
    std::uint8_t* output = destination->rgb.data() +
                           static_cast<std::size_t>(y) * source.width * 3u;
    for (std::uint32_t x = 0; x < source.width; ++x) {
      output[x * 3u + 0u] = input[x * 4u + 0u];
      output[x * 3u + 1u] = input[x * 4u + 1u];
      output[x * 3u + 2u] = input[x * 4u + 2u];
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace playback_video_chapters;
  const bool exerciseYield =
      argc == 6 && std::string_view(argv[5]) == "--exercise-yield";
  if (argc != 5 && !exerciseYield) {
    std::cerr << "usage: chapter_inference_probe <model.gguf> "
                 "<projector.gguf> <png-directory-or-video> <duration-us> "
                 "[--exercise-yield]\n";
    return 2;
  }

  std::int64_t durationUs = 0;
  try {
    durationUs = std::stoll(argv[4]);
  } catch (const std::exception&) {
    std::cerr << "invalid duration\n";
    return 2;
  }
  const std::vector<std::int64_t> sampleTimes =
      automaticChapterSampleTimes(durationUs);

  const std::filesystem::path input = argv[3];
  std::vector<OwnedFrame> frames;
  std::error_code error;
  if (std::filesystem::is_directory(input, error) && !error) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(input, error)) {
      if (error) break;
      if (entry.is_regular_file() && entry.path().extension() == ".png") {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    if (error || files.size() != sampleTimes.size()) {
      std::cerr << "expected " << sampleTimes.size()
                << " chronological PNG frames, found " << files.size() << '\n';
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
  } else if (std::filesystem::is_regular_file(input, error) && !error) {
    AnalysisRequest analysisRequest;
    analysisRequest.file = input;
    analysisRequest.videoStreamIndex = 0;
    analysisRequest.durationUs = durationUs;
    OperationControl samplingControl;
    samplingControl.cancelled = [] { return false; };
    samplingControl.backgroundGpuAllowed = [] { return true; };
    samplingControl.progress = [](std::optional<double> progress,
                                  std::string phase) {
      std::cerr << (progress ? static_cast<int>(*progress * 100.0) : -1) << "% "
                << phase << '\n';
    };
    SampledEvidenceResult sampled =
        sampleVideoEvidence(analysisRequest, samplingControl);
    if (sampled.status != OperationStatus::Succeeded ||
        sampled.frames.size() != sampleTimes.size()) {
      std::cerr << "D3D11 sampling failed: " << sampled.detail << '\n';
      return 2;
    }
    frames.reserve(sampled.frames.size());
    for (SampledFrame& frame : sampled.frames) {
      frames.push_back({frame.width, frame.height, std::move(frame.rgb)});
    }
  } else {
    std::cerr << "input is neither a PNG directory nor a video file\n";
    return 2;
  }

  InferenceRequest request;
  request.model = argv[1];
  request.projector = argv[2];
  request.durationUs = durationUs;
  request.images.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    request.images.push_back({frames[index].width,
                              frames[index].height,
                              &frames[index].rgb,
                              sampleTimes[index],
                              {}});
  }

  std::string lastPhase;
  std::atomic<bool> gpuAllowed{true};
  std::atomic<bool> yieldTriggered{false};
  OperationControl control;
  control.cancelled = [] { return false; };
  control.backgroundGpuAllowed = [&] {
    return gpuAllowed.load(std::memory_order_acquire);
  };
  control.progress = [&](std::optional<double> progress, std::string phase) {
    if (phase == lastPhase) return;
    lastPhase = phase;
    std::cerr << (progress ? static_cast<int>(*progress * 100.0) : -1) << "% "
              << phase << '\n';
    if (exerciseYield && progress && *progress >= 0.78 &&
        !yieldTriggered.exchange(true, std::memory_order_acq_rel)) {
      gpuAllowed.store(false, std::memory_order_release);
    }
  };

  InferenceEngine engine;
  InferenceCheckpoint checkpoint;
  InferenceResult inference = engine.run(request, control, &checkpoint);
  if (exerciseYield) {
    if (inference.status != OperationStatus::Yielded ||
        checkpoint.observations.empty() ||
        checkpoint.observations.size() >= request.images.size()) {
      std::cerr << "inference did not preserve a partial checkpoint at "
                   "cooperative GPU yield\n";
      return 1;
    }
    std::cerr << "YIELDED with " << checkpoint.observations.size()
              << " completed frame observations\n";
    gpuAllowed.store(true, std::memory_order_release);
    lastPhase.clear();
    inference = engine.run(request, control, &checkpoint);
  }
  if (inference.status != OperationStatus::Succeeded) {
    if (checkpoint.plan) {
      std::cerr << "PLAN\t" << checkpoint.plan->chapterCount << '\t'
                << checkpoint.plan->progression << '\n';
    }
    for (std::size_t index = 0; index < checkpoint.changePoints.size();
         ++index) {
      std::cerr << "CHANGE\t" << index + 1 << '\t'
                << checkpoint.changePoints[index].score << '\n';
    }
    std::cerr << "inference failed (" << static_cast<int>(inference.status)
              << "): " << inference.detail << '\n';
    return 1;
  }
  AnalysisResult result =
      materializeGeneratedDocument(inference.document, durationUs, sampleTimes);
  if (result.status != OperationStatus::Succeeded) {
    for (std::size_t index = 0; index < checkpoint.observations.size();
         ++index) {
      std::cerr << "EVIDENCE\t" << index + 1 << '\t'
                << checkpoint.observations[index] << '\n';
    }
    if (checkpoint.plan) {
      std::cerr << "PLAN\t" << checkpoint.plan->chapterCount << '\t'
                << checkpoint.plan->progression << '\n';
    }
    for (std::size_t index = 0; index < checkpoint.changePoints.size();
         ++index) {
      std::cerr << "CHANGE\t" << index + 1 << '\t'
                << checkpoint.changePoints[index].score << '\n';
    }
    std::cerr << "BOUNDARIES";
    for (const std::size_t frame : checkpoint.startFrames) {
      std::cerr << '\t' << frame;
    }
    std::cerr << '\n';
    for (const GeneratedChapter& chapter : inference.document.chapters) {
      std::cerr << "GENERATED_CHAPTER\t" << chapter.startFrame << '\t'
                << chapter.metadata.title << '\t' << chapter.metadata.summary
                << '\n';
    }
    std::cerr << "GENERATED_OVERVIEW\t" << inference.document.overview << '\n';
    std::cerr << "artifact rejected: " << result.detail << '\n';
    return 1;
  }

  for (std::size_t index = 0; index < checkpoint.observations.size(); ++index) {
    std::cout << "EVIDENCE\t" << index + 1 << '\t'
              << checkpoint.observations[index] << '\n';
  }
  if (checkpoint.plan) {
    std::cout << "PLAN\t" << checkpoint.plan->chapterCount << '\t'
              << checkpoint.plan->progression << '\n';
  }
  for (std::size_t index = 0; index < checkpoint.changePoints.size(); ++index) {
    std::cout << "CHANGE\t" << index + 1 << '\t'
              << checkpoint.changePoints[index].score << '\n';
  }
  std::cout << "BOUNDARIES";
  for (const std::size_t frame : checkpoint.startFrames) {
    std::cout << '\t' << frame;
  }
  std::cout << '\n';
  std::cout << "OVERVIEW\t" << result.overview << '\n';
  for (const Chapter& chapter : result.chapters) {
    std::cout << "CHAPTER\t" << chapter.startUs << '\t' << chapter.endUs << '\t'
              << chapter.title << '\t' << chapter.summary << '\n';
  }
  return 0;
}
