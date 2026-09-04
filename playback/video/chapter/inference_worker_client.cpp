#include "playback/video/chapter/inference_worker.h"
#include "playback/video/chapter/inference_worker_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/chapter/storage.h"

namespace playback_video_chapters {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(50);
constexpr DWORD kTerminatedExitCode = 0xE0000001u;
constexpr std::uintmax_t kMaximumProgressBytes = 64u * 1024u;
constexpr std::uintmax_t kMaximumResultBytes = 2u * 1024u * 1024u;

const std::filesystem::path kRequestName = L"request.json";
const std::filesystem::path kFramesName = L"frames.rgb";
const std::filesystem::path kAudioName = L"audio.f32";
const std::filesystem::path kCheckpointName = L"checkpoint.json";
const std::filesystem::path kProgressName = L"progress.json";
const std::filesystem::path kResultName = L"result.json";

struct Handle final {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

bool interrupted(const OperationControl &control) {
  try {
    return (control.cancelled && control.cancelled()) ||
           (control.backgroundGpuAllowed && !control.backgroundGpuAllowed());
  } catch (...) {
    return true;
  }
}

InferenceResult interruptionResult(const OperationControl &control) {
  try {
    if (control.cancelled && control.cancelled())
      return {OperationStatus::Cancelled, "Chapter analysis cancelled.", {}};
  } catch (...) {
    return {OperationStatus::Cancelled, "Chapter analysis cancelled.", {}};
  }
  return {OperationStatus::Yielded, "Playback reclaimed the GPU.", {}};
}

bool validSourceKey(const std::string &key) {
  return key.size() == 64 &&
         std::all_of(key.begin(), key.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::filesystem::path workspacePath(const std::string &key) {
  const std::filesystem::path root = analysisCacheRoot();
  if (!validSourceKey(key) || root.empty())
    return {};
  return root / "work" / key;
}

std::wstring quoteArgument(const std::wstring &argument) {
  if (argument.empty())
    return L"\"\"";
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return argument;
  std::wstring quoted(1, L'"');
  std::size_t slashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(slashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      slashes = 0;
      continue;
    }
    quoted.append(slashes, L'\\');
    slashes = 0;
    quoted.push_back(character);
  }
  quoted.append(slashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

void publishLatestProgress(const std::filesystem::path &workspace,
                           const OperationControl &control,
                           std::uint64_t *lastSequence) {
  if (!lastSequence || !control.progress)
    return;
  try {
    const std::filesystem::path path = workspace / kProgressName;
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size == 0 || size > kMaximumProgressBytes)
      return;
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input || !document.is_object() ||
        document.value("schema", 0) != inference_worker_protocol::kSchema)
      return;
    const std::uint64_t sequence = document.value("sequence", 0ull);
    if (sequence <= *lastSequence)
      return;
    std::optional<double> fraction;
    if (document.contains("progress") && document["progress"].is_number())
      fraction = document["progress"].get<double>();
    const std::string phase = document.value("phase", std::string{});
    *lastSequence = sequence;
    control.progress(fraction, phase);
  } catch (...) {
    // Progress is advisory. The process result remains authoritative.
  }
}

std::optional<InferenceResult>
loadResult(const std::filesystem::path &workspace) {
  try {
    const std::filesystem::path path = workspace / kResultName;
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size == 0 || size > kMaximumResultBytes)
      return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input || !document.is_object() ||
        document.value("schema", 0) != inference_worker_protocol::kSchema ||
        document.value("operation", std::string{}) != "analyze" ||
        !document.contains("chapters") || !document["chapters"].is_array())
      return std::nullopt;
    const int rawStatus = document.value("status", -1);
    if (rawStatus < static_cast<int>(OperationStatus::Succeeded) ||
        rawStatus > static_cast<int>(OperationStatus::Failed))
      return std::nullopt;
    InferenceResult result;
    result.status = static_cast<OperationStatus>(rawStatus);
    result.detail = document.value("detail", std::string{});
    for (const auto &chapter : document["chapters"]) {
      result.document.chapters.push_back(
          {chapter.at("start_us").get<std::int64_t>(),
           chapter.at("title").get<std::string>()});
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<SpeechChapterPlanResult>
loadSpeechChapterPlanResult(const std::filesystem::path &workspace) {
  try {
    const std::filesystem::path path = workspace / kResultName;
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size == 0 || size > kMaximumResultBytes)
      return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input || !document.is_object() ||
        document.value("schema", 0) != inference_worker_protocol::kSchema ||
        document.value("operation", std::string{}) !=
            "plan_chapters_from_speech" ||
        !document.contains("chapter_plan") ||
        !document["chapter_plan"].is_array()) {
      return std::nullopt;
    }
    const int rawStatus = document.value("status", -1);
    if (rawStatus < static_cast<int>(OperationStatus::Succeeded) ||
        rawStatus > static_cast<int>(OperationStatus::Failed)) {
      return std::nullopt;
    }
    SpeechChapterPlanResult result;
    result.status = static_cast<OperationStatus>(rawStatus);
    result.detail = document.value("detail", std::string{});
    for (const auto &chapter : document["chapter_plan"]) {
      result.chapterPlan.push_back(
          {chapter.at("start_us").get<std::int64_t>(),
           chapter.at("title").get<std::string>()});
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<playback_video_transcript::SpeechWorkerResult>
loadSpeechTranscriptionResult(const std::filesystem::path &workspace) {
  constexpr std::size_t kMaximumSegments = 20'000;
  constexpr std::size_t kMaximumTokens = 200'000;
  try {
    const std::filesystem::path path = workspace / kResultName;
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size == 0 || size > kMaximumResultBytes)
      return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    if (!input || !document.is_object() ||
        document.value("schema", 0) != inference_worker_protocol::kSchema ||
        document.value("operation", std::string{}) != "transcribe_speech" ||
        !document.contains("segments") || !document["segments"].is_array() ||
        document["segments"].size() > kMaximumSegments) {
      return std::nullopt;
    }
    const int rawStatus = document.value("status", -1);
    if (rawStatus < static_cast<int>(
                        playback_video_transcript::SpeechWorkerStatus::
                            Succeeded) ||
        rawStatus > static_cast<int>(
                        playback_video_transcript::SpeechWorkerStatus::Failed)) {
      return std::nullopt;
    }
    playback_video_transcript::SpeechWorkerResult result;
    result.status =
        static_cast<playback_video_transcript::SpeechWorkerStatus>(rawStatus);
    result.detail = document.value("detail", std::string{});
    result.document.sourceLanguage =
        document.value("source_language", std::string{});
    std::size_t totalTokens = 0;
    for (const auto &item : document["segments"]) {
      if (!item.is_object() || !item.contains("tokens") ||
          !item["tokens"].is_array() ||
          item["tokens"].size() > kMaximumTokens - totalTokens) {
        return std::nullopt;
      }
      playback_video_transcript::RecognizedSegment segment;
      segment.startUs = item.at("start_us").get<std::int64_t>();
      segment.endUs = item.at("end_us").get<std::int64_t>();
      segment.text = item.at("text").get<std::string>();
      totalTokens += item["tokens"].size();
      segment.tokens.reserve(item["tokens"].size());
      for (const auto &tokenItem : item["tokens"]) {
        segment.tokens.push_back(
            {tokenItem.at("start_us").get<std::int64_t>(),
             tokenItem.at("end_us").get<std::int64_t>(),
             tokenItem.at("alignment_us").get<std::int64_t>(),
             tokenItem.at("text").get<std::string>()});
      }
      result.document.segments.push_back(std::move(segment));
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

bool isKnownStagingFile(const std::filesystem::path &path) {
  const std::wstring name = path.filename().wstring();
  for (const std::filesystem::path &base :
       {kRequestName, kFramesName, kAudioName, kCheckpointName, kProgressName,
        kResultName}) {
    const std::wstring prefix = base.wstring() + L".partial-";
    if (name.compare(0, prefix.size(), prefix) == 0)
      return true;
  }
  return false;
}

void removeStaleStagingFiles(const std::filesystem::path &workspace) {
  std::error_code error;
  std::filesystem::directory_iterator entries(workspace, error);
  while (!error && entries != std::filesystem::directory_iterator()) {
    const std::filesystem::directory_entry entry = *entries;
    entries.increment(error);
    std::error_code typeError;
    if (entry.is_regular_file(typeError) && !typeError &&
        isKnownStagingFile(entry.path())) {
      std::error_code ignored;
      std::filesystem::remove(entry.path(), ignored);
    }
  }
}

std::wstring workspaceMutexName(const std::string &sourceKey) {
  return L"Local\\Radioify.ChapterInference." +
         std::wstring(sourceKey.begin(), sourceKey.end());
}

void discardWorkspaceUnlocked(const std::string &sourceKey) {
  const std::filesystem::path workspace = workspacePath(sourceKey);
  if (workspace.empty())
    return;
  removeStaleStagingFiles(workspace);
  for (const std::filesystem::path &name :
       {kRequestName, kFramesName, kAudioName, kCheckpointName, kProgressName,
        kResultName}) {
    std::error_code ignored;
    std::filesystem::remove(workspace / name, ignored);
  }
  std::error_code ignored;
  std::filesystem::remove(workspace, ignored);
}

} // namespace

InferenceWorkspaceLease::InferenceWorkspaceLease(void *mutex,
                                                 std::string sourceKey)
    : mutex_(mutex), sourceKey_(std::move(sourceKey)) {}

InferenceWorkspaceLease::~InferenceWorkspaceLease() { reset(); }

InferenceWorkspaceLease::InferenceWorkspaceLease(
    InferenceWorkspaceLease &&other) noexcept
    : mutex_(std::exchange(other.mutex_, nullptr)),
      sourceKey_(std::move(other.sourceKey_)) {}

InferenceWorkspaceLease &
InferenceWorkspaceLease::operator=(InferenceWorkspaceLease &&other) noexcept {
  if (this != &other) {
    reset();
    mutex_ = std::exchange(other.mutex_, nullptr);
    sourceKey_ = std::move(other.sourceKey_);
  }
  return *this;
}

void InferenceWorkspaceLease::reset() noexcept {
  if (mutex_) {
    const HANDLE handle = static_cast<HANDLE>(mutex_);
    ReleaseMutex(handle);
    CloseHandle(handle);
    mutex_ = nullptr;
  }
  sourceKey_.clear();
}

InferenceWorkspaceLeaseResult
acquireInferenceWorkspaceLease(const std::string &sourceKey,
                               const OperationControl &control) {
  if (workspacePath(sourceKey).empty()) {
    return {OperationStatus::Failed,
            "Could not establish a private chapter worker identity.",
            {}};
  }
  const std::wstring mutexName = workspaceMutexName(sourceKey);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
  if (!mutex) {
    return {OperationStatus::Failed,
            "Could not create the chapter worker ownership lock.",
            {}};
  }
  for (;;) {
    if (interrupted(control)) {
      CloseHandle(mutex);
      const InferenceResult interruption = interruptionResult(control);
      return {interruption.status, interruption.detail, {}};
    }
    const DWORD wait = WaitForSingleObject(mutex, 50);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      return {OperationStatus::Succeeded,
              {},
              InferenceWorkspaceLease(mutex, sourceKey)};
    }
    if (wait != WAIT_TIMEOUT) {
      CloseHandle(mutex);
      return {OperationStatus::Failed,
              "Could not acquire the chapter worker ownership lock.",
              {}};
    }
  }
}

namespace {

struct WorkerProcessOutcome {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  DWORD exitCode = 0;
};

template <typename StoreRequest>
WorkerProcessOutcome runWorkerProcess(const OperationControl &control,
                                      const InferenceWorkspaceLease &lease,
                                      StoreRequest storeRequest) {
  if (!lease) {
    return {OperationStatus::Failed,
            "Chapter inference requires exclusive workspace ownership."};
  }
  const std::string &sourceKey = lease.sourceKey();
  const std::filesystem::path workspace = workspacePath(sourceKey);
  if (workspace.empty()) {
    return {OperationStatus::Failed,
            "Could not establish a private chapter worker identity."};
  }
  std::error_code directoryError;
  std::filesystem::create_directories(workspace, directoryError);
  if (directoryError) {
    return {OperationStatus::Failed,
            "Could not create the private chapter worker directory."};
  }
  removeStaleStagingFiles(workspace);
  std::error_code ignored;
  std::filesystem::remove(workspace / kProgressName, ignored);
  ignored.clear();
  std::filesystem::remove(workspace / kResultName, ignored);
  std::string storeError;
  if (!storeRequest(workspace, &storeError)) {
    return {OperationStatus::Failed, std::move(storeError)};
  }
  if (interrupted(control)) {
    const InferenceResult result = interruptionResult(control);
    return {result.status, result.detail};
  }

  const std::filesystem::path worker =
      radioifyExecutableDir() / "radioify_chapter_worker.exe";
  if (!std::filesystem::is_regular_file(worker, ignored) || ignored) {
    return {OperationStatus::Failed,
            "The private chapter inference worker is missing."};
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) {
    return {OperationStatus::Failed,
            "Could not create the chapter inference job object."};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return {OperationStatus::Failed,
            "Could not configure the chapter inference job object."};
  }

  std::wstring command = quoteArgument(worker.wstring()) + L" " +
                         quoteArgument(workspace.wstring());
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(worker.c_str(), mutableCommand.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      worker.parent_path().c_str(), &startup, &process)) {
    return {OperationStatus::Failed,
            "Could not start the private chapter inference worker."};
  }
  Handle processHandle(process.hProcess);
  Handle threadHandle(process.hThread);
  if (!AssignProcessToJobObject(job.value, processHandle.value) ||
      ResumeThread(threadHandle.value) == static_cast<DWORD>(-1)) {
    TerminateProcess(processHandle.value, kTerminatedExitCode);
    WaitForSingleObject(processHandle.value, 5000);
    return {OperationStatus::Failed,
            "Could not attach the chapter inference worker to its job."};
  }

  std::uint64_t progressSequence = 0;
  for (;;) {
    const DWORD wait = WaitForSingleObject(
        processHandle.value, static_cast<DWORD>(kPollInterval.count()));
    publishLatestProgress(workspace, control, &progressSequence);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait != WAIT_TIMEOUT) {
      TerminateJobObject(job.value, kTerminatedExitCode);
      WaitForSingleObject(processHandle.value, 5000);
      return {OperationStatus::Failed,
              "Could not monitor the chapter inference worker."};
    }
    if (interrupted(control)) {
      TerminateJobObject(job.value, kTerminatedExitCode);
      WaitForSingleObject(processHandle.value, 5000);
      const InferenceResult result = interruptionResult(control);
      return {result.status, result.detail};
    }
  }
  publishLatestProgress(workspace, control, &progressSequence);
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(processHandle.value, &exitCode)) {
    return {OperationStatus::Failed,
            "Could not read the chapter inference worker result."};
  }
  return {OperationStatus::Succeeded, {}, exitCode};
}

} // namespace

InferenceResult runInferenceWorker(const InferenceRequest &request,
                                   const OperationControl &control,
                                   const InferenceWorkspaceLease &lease) {
  const WorkerProcessOutcome outcome = runWorkerProcess(
      control, lease, [&](const std::filesystem::path &workspace,
                          std::string *error) {
        return inference_worker_protocol::storeRequest(workspace, request,
                                                       error);
      });
  if (outcome.status != OperationStatus::Succeeded)
    return {outcome.status, outcome.detail, {}};
  const std::filesystem::path workspace = workspacePath(lease.sourceKey());
  if (const auto result = loadResult(workspace))
    return *result;
  return {OperationStatus::Failed,
          "The chapter inference worker exited without a valid result "
          "(code " +
              std::to_string(outcome.exitCode) + ").",
          {}};
}

SpeechChapterPlanResult runSpeechChapterPlanWorker(
    const SpeechChapterPlanRequest &request,
    const OperationControl &control,
    const InferenceWorkspaceLease &lease) {
  const WorkerProcessOutcome outcome = runWorkerProcess(
      control, lease, [&](const std::filesystem::path &workspace,
                          std::string *error) {
        return inference_worker_protocol::storeSpeechChapterPlanRequest(
            workspace, request, error);
      });
  if (outcome.status != OperationStatus::Succeeded)
    return {outcome.status, outcome.detail, {}};
  const std::filesystem::path workspace = workspacePath(lease.sourceKey());
  if (const auto result = loadSpeechChapterPlanResult(workspace))
    return *result;
  return {OperationStatus::Failed,
          "The chapter speech-plan worker exited without a valid result "
          "(code " +
              std::to_string(outcome.exitCode) + ").",
          {}};
}

playback_video_transcript::SpeechWorkerResult runSpeechTranscriptionWorker(
    const playback_video_transcript::SpeechWorkerRequest &request,
    const OperationControl &control, const InferenceWorkspaceLease &lease) {
  const WorkerProcessOutcome outcome = runWorkerProcess(
      control, lease, [&](const std::filesystem::path &workspace,
                          std::string *error) {
        return inference_worker_protocol::storeSpeechTranscriptionRequest(
            workspace, request, error);
      });
  if (outcome.status != OperationStatus::Succeeded) {
    playback_video_transcript::SpeechWorkerStatus status =
        playback_video_transcript::SpeechWorkerStatus::Failed;
    if (outcome.status == OperationStatus::Yielded) {
      status = playback_video_transcript::SpeechWorkerStatus::Yielded;
    } else if (outcome.status == OperationStatus::Cancelled) {
      status = playback_video_transcript::SpeechWorkerStatus::Cancelled;
    }
    return {status, outcome.detail, {}};
  }
  const std::filesystem::path workspace = workspacePath(lease.sourceKey());
  if (const auto result = loadSpeechTranscriptionResult(workspace))
    return *result;
  return {playback_video_transcript::SpeechWorkerStatus::Failed,
          "The speech worker exited without a valid result (code " +
              std::to_string(outcome.exitCode) + ").",
          {}};
}

void discardInferenceWorkerWorkspace(const InferenceWorkspaceLease &lease) {
  if (!lease)
    return;
  discardWorkspaceUnlocked(lease.sourceKey());
}

void discardInferenceWorkerWorkspace(const std::string &sourceKey) {
  const InferenceWorkspaceLeaseResult acquired =
      acquireInferenceWorkspaceLease(sourceKey, {});
  if (acquired.status == OperationStatus::Succeeded)
    discardInferenceWorkerWorkspace(acquired.lease);
}

void discardInferenceWorkerWorkspaceIfIdle(const std::string &sourceKey) {
  if (workspacePath(sourceKey).empty())
    return;
  const std::wstring mutexName = workspaceMutexName(sourceKey);
  Handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
  if (!mutex.value)
    return;
  const DWORD wait = WaitForSingleObject(mutex.value, 0);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    return;
  discardWorkspaceUnlocked(sourceKey);
  ReleaseMutex(mutex.value);
}

} // namespace playback_video_chapters
