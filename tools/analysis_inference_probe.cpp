#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <llama.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "playback/video/analysis/edit_review_backend.h"
#include "playback/video/analysis/edit_review_store.h"
#include "playback/video/analysis/evidence_preparation.h"
#include "playback/video/analysis/inference.h"
#include "playback/video/analysis/inference_worker.h"
#include "playback/video/analysis/inference_worker_protocol.h"
#include "playback/video/analysis/model.h"
#include "playback/video/analysis/text_evidence.h"
#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/decoder.h"
#include "playback/video/image_wic.h"
#include "playback/video/transcript/document.h"

namespace {

class InferenceDeadline {
public:
  InferenceDeadline(int seconds, std::atomic<bool> &cancelled) {
    if (seconds > 0)
      worker_ = std::thread([this, seconds, &cancelled] {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(seconds),
                               [&] { return done_; }))
          cancelled.store(true);
      });
  }
  ~InferenceDeadline() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool done_ = false;
  std::thread worker_;
};

bool replayReviewEvidence(
    const playback_video_analysis::JobRequest &job,
    const std::filesystem::path &fixture,
    const playback_video_analysis::AnalysisProgressCallback &progress,
    const std::atomic<bool> &cancelled,
    playback_video_analysis::ReviewJobResult *result, std::string *error) {
  using namespace playback_video_analysis;
  const auto windows = reviewWindows(job.durationUs);
  ReviewProgress recorded;
  try {
    std::ifstream input(fixture, std::ios::binary);
    const auto json = nlohmann::json::parse(input);
    const auto &observations = json.at("observations");
    if (!observations.is_array() || observations.size() != windows.size())
      throw std::runtime_error("Fixture does not cover the source duration.");
    for (const auto &item : observations) {
      const auto window = windows[recorded.observations.size()];
      ReviewObservation observation{
          window,
          item.at("frame_pts_us").get<std::vector<int64_t>>(),
          {},
          item.at("response").get<std::string>()};
      if (!parseReviewObservation(observation.rawResponse, window,
                                  observation.frameTimesUs,
                                  &observation.activities, error))
        throw std::runtime_error("Invalid observation fixture.");
      recorded.observations.push_back(std::move(observation));
    }
  } catch (const std::exception &exception) {
    *error = exception.what();
    return false;
  }
  const auto paths = resolveVideoReviewModelPaths();
  ReviewRequest request{job.sourcePath,
                        job.videoStreamIndex,
                        job.durationUs,
                        paths.model,
                        paths.projector,
                        {},
                        {}};
  if (const auto speech = loadGeneratedEnglishTextEvidence(job.sourcePath))
    request.speech = speech->cues;
  InferenceEngine engine;
  OperationControl control;
  control.cancelled = [&] { return cancelled.load(); };
  control.progress = [&](std::optional<double> fraction, std::string phase) {
    progress({fraction.value_or(0.8), std::move(phase)});
  };
  control.backgroundGpuAllowed = [&] {
    const auto budget = engine.gpuMemoryBudget();
    return budget && budget->freeBytes >= 2ull * 1024 * 1024 * 1024;
  };
  InferenceGpuLease lease;
  if (lease.acquire(control, error) != OperationStatus::Succeeded)
    return false;
  const auto budget = engine.gpuMemoryBudget();
  if (!budget || budget->freeBytes < 11ull * 1024 * 1024 * 1024) {
    *error = "Text-stage replay needs 11 GiB free GPU memory before loading.";
    return false;
  }
  std::cerr
      << "Text-stage replay; vision NOT executed; app caches unchanged.\n";
  const auto reviewed = engine.reviewVideo(request, control, recorded, {});
  if (reviewed.status != OperationStatus::Succeeded) {
    if (!reviewed.rawOutput.empty())
      std::cerr << "Rejected model output: " << reviewed.rawOutput << '\n';
    *error = reviewed.status == OperationStatus::Yielded
                 ? "GPU memory reserve became unavailable; text replay stopped."
                 : reviewed.detail;
    return false;
  }
  result->suggestions =
      assembleEditProposals(job.durationUs, reviewed.progress);
  result->visualSampleCount = 0;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  using namespace playback_video_analysis;
  if (argc == 3 && std::string_view(argv[1]) == "--speech-video") {
    JobRequest request;
    request.sourcePath = std::filesystem::u8path(argv[2]);
    InferenceEngine gpu;
    const auto budget = gpu.gpuMemoryBudget();
    if (!budget || budget->freeBytes < 2ull * 1024 * 1024 * 1024) {
      std::cerr << "Insufficient driver-reported GPU memory for speech.\n";
      return 1;
    }
    std::atomic<bool> cancelled{false};
    InferenceDeadline deadline(300, cancelled);
    OperationControl control;
    control.cancelled = [&] { return cancelled.load(); };
    control.backgroundGpuAllowed = [&] {
      const auto current = gpu.gpuMemoryBudget();
      return current && current->freeBytes >= 2ull * 1024 * 1024 * 1024;
    };
    control.progress = [](std::optional<double> fraction, std::string phase) {
      std::cerr << fraction.value_or(0.0) << " " << phase << '\n';
    };
    EvidencePreparation preparation;
    const auto result = preparation.prepare(request, control);
    std::cout << nlohmann::json{{"status", static_cast<int>(result.status)},
                                {"detail", result.detail},
                                {"cues", result.evidence
                                             ? result.evidence->cues.size()
                                             : 0}}
                     .dump(2)
              << '\n';
    return result.status == OperationStatus::Succeeded ? 0 : 1;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--review-grammar") {
    // Native constrained-decoding validation needs the tokenizer, not model
    // weights, a language context or GPU memory.
    llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    auto params = llama_model_default_params();
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    const auto path = resolveVideoReviewModelPaths().model.u8string();
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(
            std::string(path.begin(), path.end()).c_str(), params),
        &llama_model_free);
    if (!model) {
      std::cerr << "The local video model vocabulary is unavailable.\n";
      return 1;
    }
    const auto *vocab = llama_model_get_vocab(model.get());
    const auto accepts = [&](const std::string &grammar,
                             const std::string &text) {
      std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
          llama_sampler_init_grammar(vocab, grammar.c_str(), "root"),
          &llama_sampler_free);
      if (!sampler)
        return false;
      std::vector<llama_token> tokens(text.size() + 1);
      const auto count = llama_tokenize(
          vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
          static_cast<int32_t>(tokens.size()), false, false);
      if (count < 0)
        return false;
      tokens.resize(count);
      tokens.push_back(llama_vocab_eos(vocab));
      for (const auto token : tokens) {
        llama_token_data candidate{token, 0.0f, 0.0f};
        llama_token_data_array candidates{&candidate, 1, -1, false};
        llama_sampler_apply(sampler.get(), &candidates);
        if (!std::isfinite(candidate.logit))
          return false;
        llama_sampler_accept(sampler.get(), token);
      }
      return true;
    };
    const playback_video_analysis::ReviewWindow window{0, 30'000'000, 0,
                                                       30'000'000};
    const auto grammar = playback_video_analysis::reviewObservationGrammar(
        window, playback_video_analysis::reviewFrameTimes(window));
    bool ok =
        accepts(
            grammar,
            R"({"activities":[{"start":"00:00:00.000","description":"Encounter","uncertain":false},{"start":"00:00:18.000","description":"Fight continues","uncertain":false}]})") &&
        !accepts(
            grammar,
            R"({"activities":[{"start":"00:00:00.000","description":"Encounter","uncertain":false},{"start":"00:00:32.000","description":"Invalid boundary","uncertain":false}]})") &&
        !accepts(
            grammar,
            R"({"activities":[{"start":"00:00:00.000","description":"Encounter","uncertain":false},{"start":"00:00:18.000","description":"Fight","uncertain":false},{"start":"00:00:02.000","description":"Out of order","uncertain":false}]})");
    const auto valuationGrammar =
        playback_video_analysis::reviewValuationGrammar();
    ok &=
        accepts(valuationGrammar,
                R"({"reason":"Encounter","activity":"major_encounter"})") &&
        !accepts(
            valuationGrammar,
            R"({"decisions":[{"activity":"major_encounter","reason":"Additional event"}]})") &&
        !accepts(
            valuationGrammar,
            R"({"start":"00:00:10.000","activity":"routine_gameplay","reason":"Invented boundary"})");
    ok &=
        accepts(grammar,
                "{\n  \"activities\": [\n    {\"start\": \"00:00:00.000\",\n"
                "     \"description\": \"Encounter\", \"uncertain\": false}\n  "
                "]\n}") &&
        accepts(valuationGrammar,
                "{\n  \"reason\": \"The encounter "
                "continues.\",\n  \"activity\": \"major_encounter\"\n}");
    const std::vector<playback_video_analysis::ReviewAggregationRow> rows{
        {0, 1, {0, 10'000'000, "Encounter", false}, {}},
        {1, 2, {10'000'000, 20'000'000, "A different room", false}, {}}};
    const auto groupingGrammar =
        playback_video_analysis::reviewAggregationGrammar(rows);
    ok &=
        accepts(
            groupingGrammar,
            R"({"starts_new_event":true,"continuation":"A different room"})") &&
        !accepts(groupingGrammar, R"({"continuation":"Missing decision"})") &&
        !accepts(
            groupingGrammar,
            R"({"starts_new_event":[true,false],"continuation":"Multiple decisions"})");
    ok &= !accepts(
        playback_video_analysis::reviewAggregationGrammar({rows.front()}),
        R"({"starts_new_event":false,"continuation":"No prior event"})");
    std::cout << "native_review_grammar_ok=" << ok << '\n';
    return ok ? 0 : 1;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--gpu-memory") {
    InferenceEngine engine;
    const auto budget = engine.gpuMemoryBudget();
    if (!budget) {
      std::cerr << "GPU memory telemetry unavailable\n";
      return 1;
    }
    std::cout << "available_gpu_bytes=" << budget->freeBytes
              << " total_gpu_bytes=" << budget->totalBytes << '\n';
    return 0;
  }
  const bool replay =
      argc >= 2 && std::string_view(argv[1]) == "--review-evidence";
  if (argc >= 2 && (std::string_view(argv[1]) == "--review-video" || replay)) {
    bool fresh = false;
    int pauseAfter = 0;
    bool valid = argc >= (replay ? 4 : 3);
    for (int index = replay ? 4 : 3; valid && index < argc; ++index) {
      const std::string_view option(argv[index]);
      if (option == "--fresh" && !fresh && !replay)
        fresh = true;
      else if (option == "--pause-after" && pauseAfter == 0 &&
               index + 1 < argc) {
        const std::string_view value(argv[++index]);
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), pauseAfter);
        valid = parsed.ec == std::errc{} &&
                parsed.ptr == value.data() + value.size() && pauseAfter > 0 &&
                pauseAfter <= 86400;
      } else
        valid = false;
    }
    if (!valid) {
      std::cerr << "usage: analysis_inference_probe --review-video <video> "
                   "[--fresh] [--pause-after <seconds>]\n";
      std::cerr << "   or: analysis_inference_probe --review-evidence <video> "
                   "<observations.json> [--pause-after <seconds>]\n";
      return 2;
    }
    const auto file = std::filesystem::u8path(argv[2]);
    VideoMetadata metadata;
    std::string error;
    if (!probeVideoMetadata(file, &metadata, &error)) {
      std::cerr << error << '\n';
      return 2;
    }
    playback_video_analysis::JobRequest request;
    request.sourcePath = file;
    request.videoStreamIndex = metadata.videoStreamIndex;
    request.durationUs = metadata.duration100ns / 10;
    request.forceReanalysis = fresh;
    playback_video_analysis::ReviewJobResult review;
    std::atomic<bool> cancelled{false};
    InferenceDeadline deadline(pauseAfter, cancelled);
    const auto started = std::chrono::steady_clock::now();
    std::string lastPhase;
    const auto report =
        [&](const playback_video_analysis::AnalysisProgress &progress) {
          if (progress.phase != lastPhase) {
            std::cerr << progress.fraction << " " << progress.phase << '\n';
            lastPhase = progress.phase;
          }
        };
    const bool ok =
        replay ? replayReviewEvidence(request, std::filesystem::u8path(argv[3]),
                                      report, cancelled, &review, &error)
               : playback_video_analysis::reviewVideoForEditing(
                     request, report, &cancelled, &review, &error);
    nlohmann::json proposals = nlohmann::json::array();
    for (const auto &proposal : review.suggestions)
      proposals.push_back(
          {{"start_us", proposal.startUs},
           {"end_us", proposal.endUs},
           {"decision",
            playback_video_analysis::editDispositionName(proposal.disposition)},
           {"reason", proposal.reason}});
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    std::cout << nlohmann::json(
                     {{"ok", ok},
                      {"error", error},
                      {"mode", replay ? "text-stage-replay" : "video-review"},
                      {"paused", cancelled.load() && !ok},
                      {"elapsed_seconds", elapsed},
                      {"frames", review.visualSampleCount},
                      {"proposals", std::move(proposals)}})
                     .dump(2)
              << '\n';
    return ok ? 0 : 1;
  }
  std::cerr << "Use --review-video <video>, --speech-video <video>, "
               "--review-grammar or --gpu-memory.\n";
  return 2;
}
