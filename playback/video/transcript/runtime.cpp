#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "playback/video/transcript/runtime.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include "core/published_json.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {
struct Handle {
  HANDLE value;
  ~Handle() { if (value) CloseHandle(value); }
};
struct Workspace {
  std::filesystem::path path;
  ~Workspace() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
std::wstring quote(const std::filesystem::path &path) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t ch : path.wstring()) {
    if (ch == L'\\') { ++slashes; continue; }
    result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}
} // namespace

bool ensureTranscriptRuntime(RuntimeComponent component,
                             const ProgressCallback &progress,
                             const std::function<bool()> &cancelled,
                             TranscriptRuntime *runtime, std::string *error) try {
  const auto fail = [&](std::string detail) {
    if (error) *error = std::move(detail);
    return false;
  };
  if (!runtime) return fail("Missing speech runtime destination.");
  if (cancelled && cancelled()) return fail("Transcript cancelled.");
  std::filesystem::path scripts;
  for (const auto &root : radioifyResourceSearchRoots()) {
    const auto candidate = root / "transcript";
    if (std::filesystem::is_regular_file(candidate / "setup_runtime.ps1") &&
        std::filesystem::is_regular_file(candidate / "worker.py")) {
      scripts = candidate;
      break;
    }
  }
  if (scripts.empty()) return fail("Speech setup files are missing from the Radioify installation.");
  const auto cache = radioifyWritableDataDir() / "transcript-runtime";
  std::filesystem::create_directories(cache);
  Workspace workspace;
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    const auto candidate = std::filesystem::temp_directory_path() /
        ("radioify-speech-setup-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
    if (std::filesystem::create_directory(candidate)) { workspace.path = candidate; break; }
  }
  if (workspace.path.empty()) return fail("Could not create speech setup workspace.");
  wchar_t systemDirectory[MAX_PATH];
  const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
  if (!length || length >= MAX_PATH) return fail("Could not locate Windows PowerShell.");
  const auto powershell = std::filesystem::path(systemDirectory) / L"WindowsPowerShell/v1.0/powershell.exe";
  auto command = quote(powershell) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
      quote(scripts / "setup_runtime.ps1") + L" -Workspace " + quote(workspace.path) +
      L" -CacheRoot " + quote(cache) + L" -Component " +
      (component == RuntimeComponent::Quality ? L"quality" : L"whisper");
  if (const auto configured = getEnvString("RADIOIFY_TRANSCRIPT_RUNTIME"))
    command += L" -RuntimePath " + quote(pathFromUtf8String(*configured));

  Handle job{CreateJobObjectW(nullptr, nullptr)};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    return fail("Could not isolate speech setup.");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, scripts.c_str(), &startup, &process))
    return fail("Could not start speech runtime setup.");
  Handle processHandle{process.hProcess}, threadHandle{process.hThread};
  if (!AssignProcessToJobObject(job.value, processHandle.value) ||
      ResumeThread(threadHandle.value) == static_cast<DWORD>(-1)) {
    TerminateProcess(processHandle.value, 1);
    WaitForSingleObject(processHandle.value, 5000);
    return fail("Could not attach speech setup to its job.");
  }
  std::string lastPhase;
  for (;;) {
    const auto wait = WaitForSingleObject(processHandle.value, 100);
    const auto state = readPublishedJson(workspace.path / "setup-progress.json");
    if (state.is_object()) {
      const auto phase = state.value("phase", std::string());
      if (!phase.empty() && phase != lastPhase) {
        if (progress) progress({0.f, phase});
        lastPhase = phase;
      }
    }
    if ((cancelled && cancelled()) || (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0)) {
      TerminateJobObject(job.value, 1);
      WaitForSingleObject(processHandle.value, 5000);
      return fail("Speech setup cancelled; downloaded files are retained for the next attempt.");
    }
    if (wait == WAIT_OBJECT_0) break;
  }
  std::error_code ec;
  const auto log = cache / "setup.log";
  std::filesystem::copy_file(workspace.path / "setup.log", log,
                            std::filesystem::copy_options::overwrite_existing, ec);
  const auto result = readPublishedJson(workspace.path / "setup-result.json");
  DWORD exitCode = 1;
  GetExitCodeProcess(processHandle.value, &exitCode);
  if (exitCode || !result.is_object() || result.value("version", 0) != 1 || result.value("status", "") != "ok") {
    std::string detail = result.is_object() ? result.value("error", std::string("Speech setup failed.")) : "Speech setup exited without a result.";
    if (!ec) detail += " Log: " + toUtf8String(log);
    return fail(std::move(detail));
  }
  runtime->directory = pathFromUtf8String(result.at("runtime").get<std::string>());
  runtime->worker = scripts / "worker.py";
  return true;
} catch (const std::exception &exception) {
  if (error) *error = exception.what();
  return false;
}
} // namespace playback_video_transcript
