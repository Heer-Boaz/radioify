#include "windows_shell_open.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "open_file_requests.h"
#include "runtime_helpers.h"
#include "shell_open_mode.h"

namespace {

constexpr uint32_t kMaxShellOpenPayloadBytes = 64u * 1024u;

std::string trimAscii(std::string_view value) {
  size_t first = 0;
  while (first < value.size() &&
         static_cast<unsigned char>(value[first]) <= ' ') {
    ++first;
  }
  size_t last = value.size();
  while (last > first && static_cast<unsigned char>(value[last - 1]) <= ' ') {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch >= 'A' && ch <= 'Z') {
                     return static_cast<char>(ch - 'A' + 'a');
                   }
                   return static_cast<char>(ch);
                 });
  return value;
}

std::string readEnvironmentString(const wchar_t* name) {
  if (!name || name[0] == L'\0') {
    return {};
  }
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) {
    return {};
  }
  std::wstring value(required, L'\0');
  const DWORD copied =
      GetEnvironmentVariableW(name, value.data(), required);
  if (copied == 0 || copied >= required) {
    return {};
  }
  value.resize(copied);

  const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                             -1, nullptr, 0, nullptr, nullptr);
  if (utf8Length <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(utf8Length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(),
                      utf8Length, nullptr, nullptr);
  return result;
}

std::wstring shellOpenObjectSuffix() {
  DWORD sessionId = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
    sessionId = 0;
  }
  return L"Radioify.ShellOpen." + std::to_wstring(sessionId);
}

std::wstring shellOpenMutexName() {
  return L"Local\\" + shellOpenObjectSuffix() + L".Mutex";
}

std::wstring shellOpenPipeName() {
  return L"\\\\.\\pipe\\" + shellOpenObjectSuffix();
}

bool writeShellOpenPayload(std::string_view payload, DWORD timeoutMs) {
  if (payload.size() > kMaxShellOpenPayloadBytes) {
    return false;
  }

  const std::wstring pipeName = shellOpenPipeName();
  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);

  for (;;) {
    HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      const uint32_t length = static_cast<uint32_t>(payload.size());
      DWORD written = 0;
      bool ok = WriteFile(pipe, &length, sizeof(length), &written, nullptr) &&
                written == sizeof(length);
      if (ok && length > 0) {
        written = 0;
        ok = WriteFile(pipe, payload.data(), length, &written, nullptr) &&
             written == length;
      }
      FlushFileBuffers(pipe);
      CloseHandle(pipe);
      return ok;
    }

    const DWORD error = GetLastError();
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline ||
        (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)) {
      return false;
    }

    const DWORD waitMs =
        static_cast<DWORD>((std::min)(deadline - now, ULONGLONG{100}));
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(pipeName.c_str(), waitMs);
    } else {
      Sleep(waitMs);
    }
  }
}

bool readExact(HANDLE handle, void* data, DWORD size) {
  auto* cursor = static_cast<char*>(data);
  DWORD remaining = size;
  while (remaining > 0) {
    DWORD read = 0;
    if (!ReadFile(handle, cursor, remaining, &read, nullptr) || read == 0) {
      return false;
    }
    cursor += read;
    remaining -= read;
  }
  return true;
}

std::filesystem::path shellOpenConfigPath() {
  return radioifyWritableDataDir() / "radioify.ini";
}

}  // namespace

ShellOpenMode configuredWindowsShellOpenMode() {
  ShellOpenMode mode = ShellOpenMode::SameInstance;
  const std::string envMode =
      readEnvironmentString(L"RADIOIFY_SHELL_OPEN_MODE");
  if (parseShellOpenMode(envMode, mode)) {
    return mode;
  }

  std::ifstream config(shellOpenConfigPath());
  if (!config.is_open()) {
    return ShellOpenMode::SameInstance;
  }

  std::string line;
  while (std::getline(config, line)) {
    std::string trimmed = trimAscii(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
      continue;
    }

    const size_t equals = trimmed.find('=');
    if (equals == std::string::npos) {
      continue;
    }

    const std::string key =
        toLowerAscii(trimAscii(std::string_view(trimmed).substr(0, equals)));
    if (key != "shell_open_mode" && key != "shell-open-mode" &&
        key != "shellopenmode") {
      continue;
    }

    const std::string value =
        trimAscii(std::string_view(trimmed).substr(equals + 1));
    if (parseShellOpenMode(value, mode)) {
      return mode;
    }
  }

  return ShellOpenMode::SameInstance;
}

bool forwardWindowsShellOpenFile(const std::filesystem::path& file,
                                 DWORD timeoutMs) {
  if (file.empty()) {
    return false;
  }
  return writeShellOpenPayload(toUtf8String(file), timeoutMs);
}

struct WindowsShellOpenServer::Impl {
  explicit Impl(OpenFileRequests& requests) : requests(requests) {}

  ~Impl() { stop(); }

  bool start() {
    if (running.load(std::memory_order_acquire)) {
      return true;
    }

    mutex = CreateMutexW(nullptr, FALSE, shellOpenMutexName().c_str());
    if (!mutex) {
      return false;
    }

    const DWORD mutexWait = WaitForSingleObject(mutex, 0);
    if (mutexWait != WAIT_OBJECT_0 && mutexWait != WAIT_ABANDONED) {
      CloseHandle(mutex);
      mutex = nullptr;
      return false;
    }
    ownsMutex = true;

    stopRequested.store(false, std::memory_order_release);
    try {
      worker = std::thread([this]() { run(); });
    } catch (...) {
      stopRequested.store(true, std::memory_order_release);
      releaseMutex();
      return false;
    }

    running.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
    if (!running.exchange(false, std::memory_order_acq_rel)) {
      releaseMutex();
      return;
    }

    stopRequested.store(true, std::memory_order_release);
    writeShellOpenPayload({}, 500);
    if (worker.joinable()) {
      worker.join();
    }

    releaseMutex();
  }

  void run() {
    const std::wstring pipeName = shellOpenPipeName();
    while (!stopRequested.load(std::memory_order_acquire)) {
      HANDLE pipe = CreateNamedPipeW(
          pipeName.c_str(), PIPE_ACCESS_INBOUND,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
          kMaxShellOpenPayloadBytes, kMaxShellOpenPayloadBytes, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        Sleep(100);
        continue;
      }

      const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                 ? TRUE
                                 : (GetLastError() == ERROR_PIPE_CONNECTED);
      if (connected) {
        readRequest(pipe);
      }
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  }

  void readRequest(HANDLE pipe) {
    uint32_t length = 0;
    if (!readExact(pipe, &length, sizeof(length)) || length == 0 ||
        length > kMaxShellOpenPayloadBytes) {
      return;
    }

    std::string payload(length, '\0');
    if (!readExact(pipe, payload.data(), length)) {
      return;
    }

    std::filesystem::path file = pathFromUtf8String(payload);
    if (file.empty()) {
      return;
    }

    requests.post(std::move(file));
  }

  void releaseMutex() {
    if (ownsMutex && mutex) {
      ReleaseMutex(mutex);
      ownsMutex = false;
    }
    if (mutex) {
      CloseHandle(mutex);
      mutex = nullptr;
    }
  }

  HANDLE mutex = nullptr;
  bool ownsMutex = false;
  std::atomic<bool> running{false};
  std::atomic<bool> stopRequested{false};
  std::thread worker;
  OpenFileRequests& requests;
};

WindowsShellOpenServer::WindowsShellOpenServer(OpenFileRequests& requests)
    : impl_(std::make_unique<Impl>(requests)) {}

WindowsShellOpenServer::~WindowsShellOpenServer() = default;

bool WindowsShellOpenServer::start() {
  return impl_->start();
}

void WindowsShellOpenServer::stop() {
  impl_->stop();
}
