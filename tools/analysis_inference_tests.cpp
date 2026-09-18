#include "playback/video/analysis/edit_review_store.h"
#include "playback/video/analysis/inference.h"
#include "playback/video/analysis/inference_worker.h"
#include "playback/video/analysis/inference_worker_protocol.h"
#include "playback/video/analysis/scene_analysis.h"
#include "playback/video/analysis/storage.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "analysis_inference_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::string processSourceKey() {
  std::ostringstream key;
  key << std::hex << std::setfill('0') << std::setw(8) << GetCurrentProcessId();
  return std::string(56, 'f') + key.str();
}

std::wstring executablePath() {
  std::vector<wchar_t> path(32768);
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return {};
  return std::wstring(path.data(), length);
}

std::filesystem::path isolatedCacheRoot() {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error);
  if (error || temporary.empty())
    return {};
  return temporary / (L"Radioify-AnalysisInferenceTests-" +
                      std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()));
}

bool interprocessLeaseTest(const std::string &sourceKey) {
  using namespace playback_video_analysis;
  discardInferenceWorkerWorkspace(sourceKey);
  InferenceWorkspaceLeaseResult owner =
      acquireInferenceWorkspaceLease(sourceKey, {});
  if (owner.status != OperationStatus::Succeeded)
    return false;

  const std::wstring executable = executablePath();
  if (executable.empty())
    return false;
  const std::wstring wideKey(sourceKey.begin(), sourceKey.end());
  std::wstring command =
      L"\"" + executable + L"\" --lease-contender " + wideKey;
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                      nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  const DWORD whileOwned = WaitForSingleObject(process.hProcess, 250);
  owner.lease = {};
  const DWORD afterRelease = WaitForSingleObject(process.hProcess, 5000);
  DWORD exitCode = 1;
  if (afterRelease == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exitCode);
  else
    TerminateProcess(process.hProcess, 1);
  CloseHandle(process.hProcess);
  discardInferenceWorkerWorkspace(sourceKey);
  return whileOwned == WAIT_TIMEOUT && afterRelease == WAIT_OBJECT_0 &&
         exitCode == 0;
}

bool editingReviewCheckpointTest(const std::filesystem::path &directory) {
  namespace review = playback_video_analysis;
  std::filesystem::create_directories(directory);
  review::ReviewRequest request;
  request.sourcePath = directory / "review-source.mp4";
  request.videoStreamIndex = 0;
  request.durationUs = 60'000'000;
  {
    std::ofstream source(request.sourcePath);
    source << "fixture";
  }
  request.identity = review::reviewIdentity(request);
  const auto path = review::reviewCachePath(request);
  if (path.empty())
    return false;
  const auto windows = review::reviewWindows(request.durationUs);
  review::ReviewObservation first;
  first.window = windows.front();
  first.frameTimesUs = review::reviewFrameTimes(first.window);
  first.rawResponse =
      R"({"activities":[{"start":"00:00:00.000","description":"A creature attacks","uncertain":false}]})";
  std::string error;
  review::ReviewProgress loaded;
  bool ok = expect(
      review::storeReviewProgress(request, {{first}, {}, {}}, &error) &&
          review::loadReviewProgress(request, &loaded) &&
          loaded.observations.size() == 1 &&
          loaded.observations.front().rawResponse == first.rawResponse,
      "interrupted editing review resumes from exactly its completed windows");
  auto invalid = first;
  invalid.frameTimesUs.clear();
  ok &= expect(
      !review::storeReviewProgress(request, {{invalid}, {}, {}}, &error) &&
          review::loadReviewProgress(request, &loaded) &&
          loaded.observations.size() == 1,
      "invalid checkpoints cannot overwrite previously completed review work");
  invalid = first;
  for (auto &time : invalid.frameTimesUs)
    time += 1'000'000;
  ok &= expect(
      !review::storeReviewProgress(request, {{invalid}, {}, {}}, &error),
      "frames from a later interval cannot impersonate scheduled evidence");
  {
    std::ofstream corrupted(path);
    corrupted << "{\"identity\":";
  }
  ok &= expect(
      !review::loadReviewProgress(request, &loaded) &&
          loaded.observations.size() == 1,
      "a truncated cache is rejected without publishing partial parse state");
  auto second = first;
  second.window = windows.back();
  second.frameTimesUs = review::reviewFrameTimes(second.window);
  second.rawResponse =
      R"({"activities":[{"start":"00:00:28.000","description":"The creature keeps attacking","uncertain":false}]})";
  ok &= expect(
      review::storeReviewProgress(request, {{first, second}, {}, {}}, &error) &&
          review::loadReviewProgress(request, &loaded) &&
          loaded.observations.size() == 2 &&
          loaded.observations.back().window.coreEndUs == request.durationUs &&
          !review::reviewComplete(request.durationUs, loaded),
      "a completed visual checkpoint is not a completed editing review");
  loaded.aggregations.push_back(
      {1,
       R"({"starts_new_event":true,"continuation":"A creature encounter"})",
       {},
       {}});
  ok &= expect(review::storeReviewProgress(request, loaded, &error) &&
                   review::loadReviewProgress(request, &loaded) &&
                   loaded.aggregations.size() == 1 &&
                   !review::reviewComplete(request.durationUs, loaded),
               "aggregation can pause independently after its first page");
  auto invalidProgress = loaded;
  invalidProgress.aggregations.push_back(
      {1, loaded.aggregations.back().rawResponse, {}, {}});
  ok &=
      expect(!review::storeReviewProgress(request, invalidProgress, &error) &&
                 review::loadReviewProgress(request, &loaded) &&
                 loaded.aggregations.size() == 1,
             "a non-advancing aggregation cannot overwrite a valid checkpoint");
  loaded.aggregations.push_back(
      {2,
       R"({"starts_new_event":false,"continuation":"The same creature encounter continues"})",
       {},
       {}});
  ok &= expect(review::storeReviewProgress(request, loaded, &error) &&
                   review::loadReviewProgress(request, &loaded) &&
                   review::reviewGroupingComplete(request.durationUs, loaded) &&
                   !review::reviewComplete(request.durationUs, loaded) &&
                   review::reviewEvents(loaded).size() == 1 &&
                   review::reviewEvents(loaded).front().evidenceEnd == 2,
               "resumed aggregation reconstructs the continued event from "
               "validated raw output");
  loaded.valuations.push_back(
      {R"({"activity":"major_encounter","reason":"The complete encounter"})",
       review::EditDisposition::Keep, "The complete encounter"});
  ok &=
      expect(review::storeReviewProgress(request, loaded, &error) &&
                 review::loadReviewProgress(request, &loaded) &&
                 review::reviewComplete(request.durationUs, loaded),
             "the final assessment has its own validated resumable checkpoint");
  auto invalidValuation = loaded;
  invalidValuation.valuations.push_back(loaded.valuations.back());
  ok &=
      expect(!review::storeReviewProgress(request, invalidValuation, &error) &&
                 review::loadReviewProgress(request, &loaded) &&
                 loaded.valuations.size() == 1,
             "extra valuation cannot replace completed work");
  auto assessmentResume = loaded;
  assessmentResume.aggregations.back().rawResponse =
      R"({"starts_new_event":true,"continuation":"A distinct final scene"})";
  ok &= expect(
      review::storeReviewProgress(request, assessmentResume, &error) &&
          review::loadReviewProgress(request, &assessmentResume) &&
          assessmentResume.valuations.size() == 1 &&
          !review::reviewComplete(request.durationUs, assessmentResume),
      "one completed event assessment survives a pause before the final scene");
  const auto remainingEvents = review::reviewEvents(assessmentResume);
  if (remainingEvents.size() != 2 || assessmentResume.valuations.size() != 1)
    return false;
  const auto remainingInput = review::reviewValuationInput(
      remainingEvents[assessmentResume.valuations.size()],
      review::reviewEvidence(assessmentResume));
  ok &= expect(
      remainingInput &&
          remainingInput->observations.front().startUs == 30'000'000 &&
          remainingInput->observations.front().description ==
              review::reviewEvidence(assessmentResume).back().description,
      "resumed assessment obtains the next event from application-owned "
      "ordering");
  assessmentResume.valuations.push_back(
      {R"({"activity":"unclear","reason":"Check the distinct final scene"})",
       review::EditDisposition::Review, "Check the distinct final scene"});
  ok &= expect(
      review::storeReviewProgress(request, assessmentResume, &error) &&
          review::loadReviewProgress(request, &assessmentResume) &&
          review::reviewComplete(request.durationUs, assessmentResume) &&
          assessmentResume.valuations[0].reason == "The complete encounter" &&
          assessmentResume.valuations[1].reason ==
              "Check the distinct final scene",
      "resuming preserves each event's assessment and independently completes "
      "the final one");
  auto spoken = request;
  spoken.speech = {{0, 2'000'000, "Spoken objective"}};
  const auto spokenIdentity = review::reviewIdentity(spoken);
  ok &= expect(spokenIdentity != request.identity,
               "timed speech participates in review identity");
  spoken.speech[0].endUs += 1;
  ok &= expect(review::reviewIdentity(spoken) != spokenIdentity,
               "speech end timestamps participate in review identity");
  spoken.speech[0].endUs -= 1;
  spoken.speech[0].text = "Corrected objective";
  ok &= expect(review::reviewIdentity(spoken) != spokenIdentity,
               "corrected speech invalidates review identity");
  spoken.identity = review::reviewIdentity(spoken);
  const auto speechWorkspace = directory / "speech-review-request";
  std::filesystem::create_directory(speechWorkspace);
  bool speechRequestValid =
      review::inference_worker_protocol::storeVideoReviewRequest(
          speechWorkspace, spoken, &error);
  if (speechRequestValid) {
    std::ifstream input(speechWorkspace / "request.json", std::ios::binary);
    const auto document = nlohmann::json::parse(input);
    speechRequestValid =
        document.at("identity") == spoken.identity &&
        document.at("speech") ==
            nlohmann::json::array({{{"start_us", 0},
                                    {"end_us", 2'000'000},
                                    {"text", "Corrected objective"}}}) &&
        !document.contains("schema");
  }
  ok &= expect(speechRequestValid, "the private editing worker receives "
                                   "original timed speech and its identity");
  auto invalidSpeech = spoken;
  invalidSpeech.speech.front().endUs = 0;
  ok &= expect(review::reviewIdentity(invalidSpeech).empty(),
               "empty speech intervals cannot identify an editing review");
  invalidSpeech = spoken;
  invalidSpeech.speech.push_back({-1, 1, "Invalid timestamp"});
  ok &= expect(
      review::reviewIdentity(invalidSpeech).empty(),
      "negative or out-of-order speech cannot identify an editing review");
  auto changed = request;
  changed.videoStreamIndex = 1;
  ok &= expect(review::reviewIdentity(changed) != request.identity,
               "stream selection participates in editing review identity");
  auto spelling = request.sourcePath.wstring();
  if (!spelling.empty() && spelling.front() >= L'A' &&
      spelling.front() <= L'Z') {
    spelling.front() += L'a' - L'A';
    changed = request;
    changed.sourcePath = spelling;
    ok &= expect(review::reviewIdentity(changed) == request.identity,
                 "Windows path casing does not restart editing review");
  }
  const auto modified = std::filesystem::last_write_time(request.sourcePath);
  std::filesystem::rename(request.sourcePath,
                          directory / "previous-review-source.mp4");
  {
    std::ofstream replacement(request.sourcePath);
    replacement << "fixture";
  }
  std::filesystem::last_write_time(request.sourcePath, modified);
  ok &= expect(
      review::reviewIdentity(request) != request.identity,
      "replacing a same-size same-timestamp video invalidates its review");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  using namespace playback_video_analysis;
  if (argc == 3 && std::string_view(argv[1]) == "--lease-contender") {
    const InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(argv[2], {});
    return acquired.status == OperationStatus::Succeeded ? 0 : 1;
  }
  const bool requireVulkan =
      argc == 2 && std::string_view(argv[1]) == "--require-vulkan";

  const std::filesystem::path cacheRoot = isolatedCacheRoot();
  if (cacheRoot.empty() ||
      !SetEnvironmentVariableW(L"RADIOIFY_ANALYSIS_CACHE_ROOT",
                               cacheRoot.c_str())) {
    std::cerr << "analysis_inference_tests: could not isolate analysis cache\n";
    return 1;
  }

  bool ok = editingReviewCheckpointTest(cacheRoot / "editing-review");
  ok &= expect(interprocessLeaseTest(processSourceKey()),
               "a second process must wait until the complete source-level "
               "workspace transaction releases ownership");
  ok &= expect(analysisCacheRoot() == cacheRoot.lexically_normal(),
               "editing inference must honor its isolated cache root");

  const std::filesystem::path protocolWorkspace = cacheRoot / "protocol";
  std::error_code protocolError;
  std::filesystem::create_directories(protocolWorkspace, protocolError);
  std::string protocolDetail;
  std::vector<float> speechSamples(1600, 0.25f);
  playback_video_transcript::SpeechWorkerRequest speechProtocol;
  speechProtocol.model = "whisper.bin";
  speechProtocol.alignmentPreset =
      playback_video_transcript::WhisperAlignmentPreset::Base;
  speechProtocol.task =
      playback_video_transcript::WhisperTask::TranslateToEnglish;
  speechProtocol.samples = speechSamples.data();
  speechProtocol.sampleCount = speechSamples.size();
  bool speechProtocolValid =
      inference_worker_protocol::storeSpeechTranscriptionRequest(
          protocolWorkspace, speechProtocol, &protocolDetail);
  if (speechProtocolValid) {
    try {
      std::ifstream input(protocolWorkspace / "request.json", std::ios::binary);
      nlohmann::json document;
      input >> document;
      speechProtocolValid =
          input && document.is_object() &&
          document.value("operation", std::string{}) == "transcribe_speech" &&
          document.value("sample_count", std::size_t{0}) ==
              speechSamples.size() &&
          std::filesystem::file_size(protocolWorkspace / "audio.f32") ==
              speechSamples.size() * sizeof(float);
    } catch (...) {
      speechProtocolValid = false;
    }
  }
  ok &= expect(speechProtocolValid,
               "the killable worker protocol must preserve an exact owned "
               "PCM chunk without media seeking or text serialization");

  InferenceEngine engine;
  OperationControl cancelled;
  cancelled.cancelled = [] { return true; };
  cancelled.backgroundGpuAllowed = [] { return true; };
  ok &= expect(engine.inspect(cancelled).state == CapabilityState::Cancelled,
               "cancellation must win before backend initialization");
  ok &= expect(engine.reviewVideo({}, cancelled, {}, {}).status ==
                   OperationStatus::Cancelled,
               "editing review cancellation must win before source validation "
               "or model loading");
  OperationControl yielded;
  yielded.cancelled = [] { return false; };
  yielded.backgroundGpuAllowed = [] { return false; };
  ok &= expect(engine.inspect(yielded).state == CapabilityState::Yielded,
               "unavailable GPU memory must prevent backend initialization");

  OperationControl available;
  available.cancelled = [] { return false; };
  available.backgroundGpuAllowed = [] { return true; };
  const CapabilityResult capability = engine.inspect(available);
  ok &= expect(capability.state == CapabilityState::Ready ||
                   (capability.state == CapabilityState::Unsupported &&
                    !capability.detail.empty()),
               "device inspection must return a typed, explained result");
  if (requireVulkan) {
    ok &= expect(capability.state == CapabilityState::Ready,
                 "the requested Vulkan integration probe must initialize");
  }

  std::error_code cleanupError;
  std::filesystem::remove_all(cacheRoot, cleanupError);
  return ok ? 0 : 1;
}
