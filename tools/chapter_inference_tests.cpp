#include "playback/video/chapter/inference.h"
#include "playback/video/chapter/inference_worker.h"
#include "playback/video/chapter/inference_worker_protocol.h"
#include "playback/video/chapter/storage.h"
#include "playback/video/analysis/scene_analysis.h"

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
    std::cerr << "chapter_inference_tests: " << message << '\n';
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
  return temporary /
         (L"Radioify-ChapterInferenceTests-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(GetTickCount64()));
}

bool interprocessLeaseTest(const std::string &sourceKey) {
  using namespace playback_video_chapters;
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

} // namespace

int main(int argc, char **argv) {
  using namespace playback_video_chapters;
  if (argc == 3 && std::string_view(argv[1]) == "--lease-contender") {
    const InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(argv[2], {});
    return acquired.status == OperationStatus::Succeeded ? 0 : 1;
  }
  const bool requireVulkan =
      argc == 2 && std::string_view(argv[1]) == "--require-vulkan";

  const std::filesystem::path cacheRoot = isolatedCacheRoot();
  if (cacheRoot.empty() ||
      !SetEnvironmentVariableW(L"RADIOIFY_CHAPTER_CACHE_ROOT",
                               cacheRoot.c_str())) {
    std::cerr << "chapter_inference_tests: could not isolate chapter cache\n";
    return 1;
  }

  InferenceRequest boundedPrompt;
  boundedPrompt.durationUs = kMaximumAutomaticChapterVideoDurationUs;
  boundedPrompt.englishDialogue.push_back(
      {0, "Opening narration establishes the video topic."});
  boundedPrompt.windows.resize(kMaximumAutomaticEvidenceSampleCount);
  std::vector<std::int64_t> expectedSampleTimesUs;
  expectedSampleTimesUs.reserve(boundedPrompt.windows.size());
  for (std::size_t index = 0; index < boundedPrompt.windows.size(); ++index) {
    const std::int64_t startUs =
        boundedPrompt.durationUs * static_cast<std::int64_t>(index) /
        static_cast<std::int64_t>(boundedPrompt.windows.size());
    boundedPrompt.chapterPlan.push_back(
        {startUs, "Chapter " + std::to_string(index + 1)});
    expectedSampleTimesUs.push_back(startUs);
  }
  expectedSampleTimesUs.front() = 1'000'000;
  for (std::size_t windowIndex = 0; windowIndex < boundedPrompt.windows.size();
       ++windowIndex) {
    InferenceTemporalWindow &window = boundedPrompt.windows[windowIndex];
    window.intervalStartUs =
        windowIndex == 0
            ? 0
            : expectedSampleTimesUs[windowIndex - 1] +
                  (expectedSampleTimesUs[windowIndex] -
                   expectedSampleTimesUs[windowIndex - 1]) /
                      2;
    window.intervalEndUs =
        windowIndex + 1 == boundedPrompt.windows.size()
            ? boundedPrompt.durationUs
            : expectedSampleTimesUs[windowIndex] +
                  (expectedSampleTimesUs[windowIndex + 1] -
                   expectedSampleTimesUs[windowIndex]) /
                      2;
    window.frames.resize(1);
    window.frames.front().timeUs = expectedSampleTimesUs[windowIndex];
  }
  std::string budgetError;
  bool ok = expect(validateInferenceInputBudget(boundedPrompt, &budgetError),
                   "the complete sixty-minute visual evidence envelope must "
                   "fit the planner context by construction");
  for (std::int64_t index = 0; index < 100; ++index) {
    boundedPrompt.englishDialogue.push_back(
        {index * 1'000, std::string(600, 'x')});
  }
  ok &= expect(!validateInferenceInputBudget(boundedPrompt, &budgetError) &&
                   !budgetError.empty(),
               "over-budget subtitle evidence must be rejected before model "
               "loading");
  ok &= expect(interprocessLeaseTest(processSourceKey()),
               "a second process must wait until the complete source-level "
               "workspace transaction releases ownership");
  ok &= expect(analysisCacheRoot() == cacheRoot.lexically_normal(),
               "chapter inference must honor its isolated cache root");

  const std::filesystem::path protocolWorkspace = cacheRoot / "protocol";
  std::error_code protocolError;
  std::filesystem::create_directories(protocolWorkspace, protocolError);
  SpeechChapterPlanRequest selectionProtocol;
  selectionProtocol.plannerModel = "planner.gguf";
  selectionProtocol.planAdapter = "plan.gguf";
  selectionProtocol.durationUs = 60'000'000;
  selectionProtocol.englishDialogue.push_back(
      {0, "The introduction establishes the topic."});
  std::string protocolDetail;
  bool planProtocolValid =
      !protocolError &&
      inference_worker_protocol::storeSpeechChapterPlanRequest(
          protocolWorkspace, selectionProtocol, &protocolDetail);
  if (planProtocolValid) {
    try {
      std::ifstream input(protocolWorkspace / "request.json",
                          std::ios::binary);
      nlohmann::json document;
      input >> document;
      planProtocolValid =
          input && document.is_object() &&
          document.value("schema", 0) == inference_worker_protocol::kSchema &&
          document.value("operation", std::string{}) ==
              "plan_chapters_from_speech" &&
          document.value("duration_us", std::int64_t{0}) == 60'000'000 &&
          document["english_dialogue"].size() == 1;
    } catch (const nlohmann::json::exception &) {
      planProtocolValid = false;
    }
  }
  ok &= expect(planProtocolValid,
               "the worker protocol must preserve the typed ASR planner "
               "operation independently of frame-analysis requests");

  InferenceEngine engine;
  OperationControl cancelled;
  cancelled.cancelled = [] { return true; };
  cancelled.backgroundGpuAllowed = [] { return true; };
  ok &= expect(engine.inspect(cancelled).state == CapabilityState::Cancelled,
               "cancellation must win before backend initialization");
  SpeechChapterPlanRequest cancelledPlan;
  const SpeechChapterPlanResult cancelledPlanResult =
      engine.planChaptersFromSpeech(cancelledPlan, cancelled);
  ok &= expect(cancelledPlanResult.status == OperationStatus::Cancelled,
               "speech-guided planning must honor cancellation before "
               "model validation or loading");

  OperationControl yielded;
  yielded.cancelled = [] { return false; };
  yielded.backgroundGpuAllowed = [] { return false; };
  ok &= expect(engine.inspect(yielded).state == CapabilityState::Yielded,
               "foreground playback must prevent backend initialization");

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

  InferenceRequest invalid;
  const InferenceResult invalidResult = engine.run(invalid, available);
  if (capability.state == CapabilityState::Ready) {
    ok &= expect(invalidResult.status == OperationStatus::Failed &&
                     !invalidResult.detail.empty(),
                 "an incomplete in-memory request must fail before loading a "
                 "model");
  } else {
    ok &= expect(invalidResult.status == OperationStatus::Unsupported,
                 "unsupported hardware must remain an explicit result");
  }
  std::error_code cleanupError;
  std::filesystem::remove_all(cacheRoot, cleanupError);
  return ok ? 0 : 1;
}
