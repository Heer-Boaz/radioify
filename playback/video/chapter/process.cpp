#include "playback/video/chapter/process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "core/windows_handle.h"

namespace playback_video_chapters {
namespace {

std::wstring quote(const std::wstring& value) {
  if (value.empty()) return L"\"\"";
  if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return value;
  }
  std::wstring out = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    if (ch == L'\"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(ch);
      slashes = 0;
      continue;
    }
    out.append(slashes, L'\\');
    slashes = 0;
    out.push_back(ch);
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'\"');
  return out;
}

void drainPipe(HANDLE pipe, std::string* output, std::size_t limit) {
  if (!pipe || !output) return;
  char buffer[16 * 1024];
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ||
        available == 0) {
      return;
    }
    const DWORD requested = static_cast<DWORD>(
        std::min<std::size_t>(sizeof(buffer), available));
    DWORD read = 0;
    if (!ReadFile(pipe, buffer, requested, &read, nullptr) || read == 0) {
      return;
    }
    const std::size_t remaining =
        output->size() < limit ? limit - output->size() : 0;
    output->append(buffer, buffer + std::min<std::size_t>(remaining, read));
  }
}

void stopProcess(HANDLE job, HANDLE process) {
  if (job && TerminateJobObject(job, ERROR_CANCELLED)) return;
  if (process) TerminateProcess(process, ERROR_CANCELLED);
}

}  // namespace

ProcessResult runHiddenProcess(const std::filesystem::path& executable,
                               const std::vector<std::wstring>& arguments,
                               const OperationControl& control,
                               bool yieldForPlayback,
                               std::size_t maximumCapturedBytes) {
  ProcessResult result;
  if (control.cancelled && control.cancelled()) {
    result.status = OperationStatus::Cancelled;
    result.detail = "Chapter analysis cancelled.";
    return result;
  }
  if (yieldForPlayback && control.backgroundGpuAllowed &&
      !control.backgroundGpuAllowed()) {
    result.status = OperationStatus::Yielded;
    result.detail = "Playback has GPU priority.";
    return result;
  }
  if (executable.empty()) {
    result.status = OperationStatus::Unsupported;
    result.detail = "The chapter analysis helper is missing.";
    return result;
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE rawRead = nullptr;
  HANDLE rawWrite = nullptr;
  if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) {
    result.detail = "Could not create chapter-engine output pipes.";
    return result;
  }
  UniqueWindowsHandle readPipe(rawRead);
  UniqueWindowsHandle writePipe(rawWrite);
  SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);

  std::wstring command = quote(executable.wstring());
  for (const std::wstring& argument : arguments) {
    command.push_back(L' ');
    command += quote(argument);
  }
  std::vector<wchar_t> commandLine(command.begin(), command.end());
  commandLine.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = writePipe.get();
  startup.hStdError = writePipe.get();
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  UniqueWindowsHandle job(CreateJobObjectW(nullptr, nullptr));
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      job.reset();
    }
  }
  PROCESS_INFORMATION processInfo{};
  if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr,
                      nullptr, TRUE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED |
                          BELOW_NORMAL_PRIORITY_CLASS,
                      nullptr,
                      executable.parent_path().c_str(), &startup,
                      &processInfo)) {
    result.detail = "Could not start the chapter analysis helper.";
    return result;
  }
  UniqueWindowsHandle process(processInfo.hProcess);
  UniqueWindowsHandle thread(processInfo.hThread);
  writePipe.reset();

  if (job && !AssignProcessToJobObject(job.get(), process.get())) {
    job.reset();
  }
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    stopProcess(job.get(), process.get());
    WaitForSingleObject(process.get(), 5000);
    result.detail = "Could not resume the chapter analysis helper.";
    return result;
  }

  for (;;) {
    drainPipe(readPipe.get(), &result.output, maximumCapturedBytes);
    if (control.cancelled && control.cancelled()) {
      stopProcess(job.get(), process.get());
      WaitForSingleObject(process.get(), 5000);
      drainPipe(readPipe.get(), &result.output, maximumCapturedBytes);
      result.status = OperationStatus::Cancelled;
      result.detail = "Chapter analysis cancelled.";
      return result;
    }
    if (yieldForPlayback && control.backgroundGpuAllowed &&
        !control.backgroundGpuAllowed()) {
      stopProcess(job.get(), process.get());
      WaitForSingleObject(process.get(), 5000);
      drainPipe(readPipe.get(), &result.output, maximumCapturedBytes);
      result.status = OperationStatus::Yielded;
      result.detail = "Playback reclaimed the GPU.";
      return result;
    }
    const DWORD wait = WaitForSingleObject(process.get(), 25);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_FAILED) {
      stopProcess(job.get(), process.get());
      result.detail = "Could not monitor the chapter analysis helper.";
      return result;
    }
  }
  drainPipe(readPipe.get(), &result.output, maximumCapturedBytes);
  DWORD exitCode = ERROR_GEN_FAILURE;
  GetExitCodeProcess(process.get(), &exitCode);
  result.exitCode = exitCode;
  if (exitCode != 0) {
    result.detail = "The chapter analysis helper exited with code " +
                    std::to_string(exitCode) + ".";
    return result;
  }
  result.status = OperationStatus::Succeeded;
  return result;
}

}  // namespace playback_video_chapters
