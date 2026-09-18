#include "playback/video/analysis/edit_review_store.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/file_instance.h"
#include "core/file_output.h"
#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "core/sha256.h"
#include "core/utf8.h"
#include "playback/video/analysis/model.h"
#include "playback/video/analysis/runtime_contract.h"

namespace playback_video_analysis {
namespace {

// A bounded raw response can double in size when escaped inside JSON. Include
// its frame timestamps and document overhead, with the same bound on
// read/write.
uintmax_t cacheByteLimit(size_t windows) {
  return 4096 + windows * 256ull * 1024;
}

bool validObservation(ReviewObservation *observation,
                      const ReviewWindow &window, int64_t durationUs) {
  const auto targets = reviewFrameTimes(window);
  if (targets.empty() || observation->frameTimesUs.size() != targets.size() ||
      !std::is_sorted(observation->frameTimesUs.begin(),
                      observation->frameTimesUs.end()) ||
      observation->frameTimesUs.front() < 0 ||
      observation->frameTimesUs.back() >= durationUs)
    return false;
  for (size_t index = 0; index < targets.size(); ++index)
    if (observation->frameTimesUs[index] > targets[index] + kReviewFrameStepUs)
      return false;
  return parseReviewObservation(observation->rawResponse, window,
                                observation->frameTimesUs,
                                &observation->activities, nullptr);
}

bool appendAggregation(const nlohmann::json &item,
                       const std::vector<ReviewEvidence> &evidence,
                       ReviewProgress *progress) {
  if (!item.is_object() || !item.contains("evidence_end") ||
      !item["evidence_end"].is_number_unsigned())
    return false;
  ReviewAggregation aggregation;
  aggregation.evidenceEnd = item.at("evidence_end").get<size_t>();
  aggregation.rawResponse = item.at("response").get<std::string>();
  const auto rows =
      reviewAggregationRows(evidence, *progress, aggregation.evidenceEnd);
  if (!parseReviewAggregation(aggregation.rawResponse, rows, &aggregation,
                              nullptr))
    return false;
  progress->aggregations.push_back(std::move(aggregation));
  return true;
}

bool appendValuation(const nlohmann::json &item, ReviewProgress *progress) {
  if (!item.is_string())
    return false;
  ReviewValuation valuation;
  if (!parseReviewValuation(item.get<std::string>(), &valuation, nullptr))
    return false;
  progress->valuations.push_back(std::move(valuation));
  return true;
}

nlohmann::json
samplingIdentity(const playback_video_analysis::QwenSampling &sampling) {
  return {{"top_k", sampling.topK},
          {"top_p", sampling.topP},
          {"temperature", sampling.temperature},
          {"repeat_penalty", sampling.repeatPenalty},
          {"frequency_penalty", sampling.frequencyPenalty},
          {"presence_penalty", sampling.presencePenalty},
          {"seed", sampling.seed}};
}

} // namespace

std::string reviewIdentity(const ReviewRequest &request) {
  std::error_code ec;
  const auto path = std::filesystem::weakly_canonical(request.sourcePath, ec);
  if (ec || path.empty())
    return {};
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    return {};
  const auto modified = std::filesystem::last_write_time(path, ec);
  if (ec)
    return {};
  const auto instance = fileInstanceIdentity(path);
  if (!instance)
    return {};
  const std::vector<ReviewAggregationRow> aggregationExample = {
      {0, 1, {0, kReviewWindowUs, "observation", false}, {}},
      {1, 2, {kReviewWindowUs, 2 * kReviewWindowUs, "event", true}, "latest"}};
  const ReviewWindow observationExample{0, kReviewWindowUs, 0, kReviewWindowUs};
  const ReviewValuationInput valuationExample{
      {{kReviewWindowUs, 2 * kReviewWindowUs, "observation", false}},
      ReviewEvidence{0, kReviewWindowUs, "preceding context", false},
      ReviewEvidence{2 * kReviewWindowUs, 3 * kReviewWindowUs,
                     "following context", false}};
  nlohmann::json speech = nlohmann::json::array();
  int64_t previousStartUs = -1;
  for (const auto &cue : request.speech) {
    if (cue.startUs < 0 || cue.startUs < previousStartUs ||
        cue.endUs <= cue.startUs || cue.text.empty() || !isValidUtf8(cue.text))
      return {};
    speech.push_back(
        {{"start_us", cue.startUs}, {"end_us", cue.endUs}, {"text", cue.text}});
    previousStartUs = cue.startUs;
  }
  const nlohmann::json identity = {
      {"task", "gameplay-archive-review"},
      {"source", toUtf8String(pathIdentityKey(makePathIdentity(path)))},
      {"size", size},
      {"modified", modified.time_since_epoch().count()},
      {"device", instance->device},
      {"file", instance->file},
      {"duration", request.durationUs},
      {"speech", std::move(speech)},
      {"stream", request.videoStreamIndex},
      {"model", playback_video_analysis::kVideoReviewModelSha256},
      {"projector", playback_video_analysis::kVideoReviewProjectorSha256},
      {"runtime", playback_video_analysis::kInferenceRuntimeRevision},
      {"observation_prompt", reviewObservationPrompt(observationExample)},
      {"observation_grammar",
       reviewObservationGrammar(observationExample,
                                reviewFrameTimes(observationExample))},
      {"aggregation_prompts",
       {reviewAggregationPrompt(aggregationExample, false),
        reviewAggregationPrompt(aggregationExample, true)}},
      {"aggregation_grammar", reviewAggregationGrammar(aggregationExample)},
      {"valuation_prompt", reviewValuationPrompt(valuationExample)},
      {"valuation_grammar", reviewValuationGrammar()},
      {"vision_sampling",
       samplingIdentity(playback_video_analysis::kQwenVisionSampling)},
      {"text_sampling",
       samplingIdentity(playback_video_analysis::kQwenTextSampling)},
      {"observation_output_tokens", kReviewObservationTokens},
      {"aggregation_output_tokens", kReviewAggregationTokens},
      {"valuation_output_tokens", kReviewValuationTokens},
      {"frame_step", kReviewFrameStepUs},
      {"window", kReviewWindowUs},
      {"handles", kReviewContextUs},
      {"image_tokens", kReviewImageTokens},
      {"context_tokens", kReviewContextTokens}};
  return core_sha256::text(identity.dump());
}

std::filesystem::path reviewCachePath(const ReviewRequest &request) {
  if (request.identity.size() != 64 ||
      request.identity.find_first_not_of("0123456789abcdef") !=
          std::string::npos)
    return {};
  return radioifyWritableDataDir() / "cache" / "video-edit-review" /
         (request.identity + ".json");
}

bool loadReviewProgress(const ReviewRequest &request,
                        ReviewProgress *progress) {
  if (!progress)
    return false;
  const auto path = reviewCachePath(request);
  const auto windows = reviewWindows(request.durationUs);
  if (path.empty() || windows.empty())
    return false;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > cacheByteLimit(windows.size()))
    return false;
  try {
    std::ifstream input(path, std::ios::binary);
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if (!json.is_object() ||
        json.value("identity", std::string{}) != request.identity ||
        !json.contains("observations") || !json["observations"].is_array() ||
        json["observations"].size() > windows.size() ||
        !json.contains("aggregations") || !json["aggregations"].is_array() ||
        !json.contains("valuations") || !json["valuations"].is_array())
      return false;
    ReviewProgress parsed;
    for (const auto &item : json["observations"]) {
      ReviewObservation observation;
      observation.window = windows[parsed.observations.size()];
      observation.rawResponse = item.at("response").get<std::string>();
      observation.frameTimesUs =
          item.at("frame_pts_us").get<std::vector<int64_t>>();
      if (!validObservation(&observation, observation.window,
                            request.durationUs))
        return false;
      parsed.observations.push_back(std::move(observation));
    }
    const auto evidence = reviewEvidence(parsed);
    if (!json["aggregations"].empty() &&
        parsed.observations.size() != windows.size())
      return false;
    if (json["aggregations"].size() > evidence.size())
      return false;
    for (const auto &item : json["aggregations"])
      if (!appendAggregation(item, evidence, &parsed))
        return false;
    if (!json["valuations"].empty() &&
        !reviewGroupingComplete(request.durationUs, parsed))
      return false;
    if (json["valuations"].size() > reviewEvents(parsed).size())
      return false;
    for (const auto &item : json["valuations"])
      if (!appendValuation(item, &parsed))
        return false;
    *progress = std::move(parsed);
    return true;
  } catch (const nlohmann::json::exception &) {
    return false;
  }
}

bool storeReviewProgress(const ReviewRequest &request,
                         const ReviewProgress &progress, std::string *error) {
  const auto &observations = progress.observations;
  const auto path = reviewCachePath(request);
  const auto windows = reviewWindows(request.durationUs);
  if (path.empty() || windows.empty() || observations.size() > windows.size()) {
    if (error)
      *error = "The video review cache identity is invalid.";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error)
      *error = "Could not create the video review cache directory.";
    return false;
  }
  nlohmann::json entries = nlohmann::json::array();
  ReviewProgress validated;
  for (size_t index = 0; index < observations.size(); ++index) {
    auto observation = observations[index];
    const auto &expected = windows[index];
    if (observation.window.startUs != expected.startUs ||
        observation.window.endUs != expected.endUs ||
        observation.window.coreStartUs != expected.coreStartUs ||
        observation.window.coreEndUs != expected.coreEndUs ||
        !validObservation(&observation, expected, request.durationUs)) {
      if (error)
        *error = "Could not save invalid video review progress.";
      return false;
    }
    entries.push_back({{"frame_pts_us", observation.frameTimesUs},
                       {"response", observation.rawResponse}});
    validated.observations.push_back(std::move(observation));
  }
  nlohmann::json aggregations = nlohmann::json::array();
  const auto evidence = reviewEvidence(validated);
  if (!progress.aggregations.empty() && observations.size() != windows.size()) {
    if (error)
      *error = "Event aggregation requires the completed visual observations.";
    return false;
  }
  for (const auto &page : progress.aggregations) {
    const nlohmann::json item = {{"evidence_end", page.evidenceEnd},
                                 {"response", page.rawResponse}};
    if (!appendAggregation(item, evidence, &validated)) {
      if (error)
        *error = "Could not save invalid event aggregation progress.";
      return false;
    }
    aggregations.push_back(item);
  }
  if ((!progress.valuations.empty() &&
       !reviewGroupingComplete(request.durationUs, validated)) ||
      progress.valuations.size() > reviewEvents(validated).size()) {
    if (error)
      *error = "Editing assessments must belong to completed events.";
    return false;
  }
  nlohmann::json valuations = nlohmann::json::array();
  for (const auto &valuation : progress.valuations) {
    const nlohmann::json item = valuation.rawResponse;
    if (!appendValuation(item, &validated)) {
      if (error)
        *error = "Could not save invalid event assessment progress.";
      return false;
    }
    valuations.push_back(item);
  }
  const nlohmann::json document = {{"identity", request.identity},
                                   {"observations", std::move(entries)},
                                   {"aggregations", std::move(aggregations)},
                                   {"valuations", std::move(valuations)}};
  const auto serialized = document.dump();
  if (serialized.size() > cacheByteLimit(windows.size())) {
    if (error)
      *error = "The video review progress exceeds its storage budget.";
    return false;
  }
  auto output = file_output::Transaction::begin(
      path, file_output::PublishMode::ReplaceExisting, error);
  if (!output)
    return false;
  std::ofstream stream(output->temporaryPath(), std::ios::binary);
  stream << serialized;
  stream.flush();
  stream.close();
  if (!stream) {
    if (error)
      *error = "Could not save video review progress.";
    return false;
  }
  return output->publish(error);
}

} // namespace playback_video_analysis
