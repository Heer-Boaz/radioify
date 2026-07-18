#include "windows_shell_open.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "open_file_requests.h"
#include "runtime_helpers.h"
#include "shell_open_mode.h"
#include "windows_app_resources.h"
#include "windows_handle.h"

namespace {

constexpr uint32_t kMaxShellOpenPayloadBytes = 64u * 1024u;
constexpr uint32_t kShellOpenNonAsciiVideo = 1u << 0;
constexpr uint32_t kKnownShellOpenFlags = kShellOpenNonAsciiVideo;

class ShellOpenSingleInstanceLock {
 public:
  ShellOpenSingleInstanceLock() = default;
  ~ShellOpenSingleInstanceLock() { reset(); }

  ShellOpenSingleInstanceLock(const ShellOpenSingleInstanceLock&) = delete;
  ShellOpenSingleInstanceLock& operator=(const ShellOpenSingleInstanceLock&) =
      delete;

  bool acquire();
  void reset();

 private:
  UniqueWindowsHandle mutex_;
  bool ownsMutex_ = false;
};

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
  return RADIOIFY_APP_NAME_W L".ShellOpen.2." + std::to_wstring(sessionId);
}

std::wstring shellOpenMutexName() {
  return L"Local\\" + shellOpenObjectSuffix() + L".Mutex";
}

std::wstring shellOpenPipeName() {
  return L"\\\\.\\pipe\\" + shellOpenObjectSuffix();
}

bool ShellOpenSingleInstanceLock::acquire() {
  reset();

  UniqueWindowsHandle mutex(
      CreateMutexW(nullptr, FALSE, shellOpenMutexName().c_str()));
  if (!mutex) {
    return false;
  }

  const DWORD waitResult = WaitForSingleObject(mutex.get(), 0);
  if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
    return false;
  }

  mutex_ = std::move(mutex);
  ownsMutex_ = true;
  return true;
}

void ShellOpenSingleInstanceLock::reset() {
  if (ownsMutex_ && mutex_) {
    ReleaseMutex(mutex_.get());
    ownsMutex_ = false;
  }
  mutex_.reset();
}

bool writeShellOpenRequest(std::string_view payload,
                           bool nonAsciiVideo,
                           uint32_t timeoutMs) {
  if (payload.size() > kMaxShellOpenPayloadBytes) {
    return false;
  }

  const std::wstring pipeName = shellOpenPipeName();
  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);

  for (;;) {
    UniqueWindowsHandle pipe(CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0,
                                         nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
    if (pipe) {
      const uint32_t flags = nonAsciiVideo ? kShellOpenNonAsciiVideo : 0;
      const uint32_t length = static_cast<uint32_t>(payload.size());
      DWORD written = 0;
      bool ok = WriteFile(pipe.get(), &flags, sizeof(flags), &written,
                          nullptr) &&
                written == sizeof(flags);
      if (ok) {
        written = 0;
        ok = WriteFile(pipe.get(), &length, sizeof(length), &written,
                       nullptr) &&
                written == sizeof(length);
      }
      if (ok && length > 0) {
        written = 0;
        ok = WriteFile(pipe.get(), payload.data(), length, &written, nullptr) &&
             written == length;
      }
      FlushFileBuffers(pipe.get());
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
                                 bool nonAsciiVideo,
                                 uint32_t timeoutMs) {
  if (file.empty()) {
    return false;
  }
  return writeShellOpenRequest(toUtf8String(file), nonAsciiVideo, timeoutMs);
}

struct WindowsShellOpenServer::Impl {
  explicit Impl(OpenFileRequests& requests) : requests(requests) { start(); }

  ~Impl() { stop(); }

  bool isAcceptingHandoffs() const { return started; }

 private:
  void start() {
    if (started) {
      return;
    }

    if (!singleInstanceLock.acquire()) {
      return;
    }

    stopEvent =
        UniqueWindowsHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent) {
      singleInstanceLock.reset();
      return;
    }

    stopRequested.store(false, std::memory_order_release);
    try {
      worker = std::thread([this]() { run(); });
    } catch (...) {
      stopRequested.store(true, std::memory_order_release);
      stopEvent.reset();
      singleInstanceLock.reset();
      return;
    }

    started = true;
  }

  void stop() {
    if (!started) {
      return;
    }

    started = false;
    stopRequested.store(true, std::memory_order_release);
    SetEvent(stopEvent.get());
    if (worker.joinable()) {
      worker.join();
    }

    stopEvent.reset();
    singleInstanceLock.reset();
  }

  void run() {
    const std::wstring pipeName = shellOpenPipeName();
    while (!stopRequested.load(std::memory_order_acquire)) {
      UniqueWindowsHandle pipe(CreateNamedPipeW(
          pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
          kMaxShellOpenPayloadBytes, kMaxShellOpenPayloadBytes, 0, nullptr));
      if (!pipe) {
        waitBeforeRetry();
        continue;
      }

      if (connectPipe(pipe.get())) {
        readRequest(pipe.get());
        DisconnectNamedPipe(pipe.get());
      }
    }
  }

  void waitBeforeRetry() {
    WaitForSingleObject(stopEvent.get(), 100);
  }

  bool waitForPipeIo(HANDLE pipe, OVERLAPPED& overlapped,
                     DWORD& transferred) {
    HANDLE handles[] = {overlapped.hEvent, stopEvent.get()};
    const DWORD waitResult =
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
      return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) !=
             FALSE;
    }
    if (waitResult == WAIT_OBJECT_0 + 1) {
      DWORD ignored = 0;
      CancelIoEx(pipe, &overlapped);
      GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
      return false;
    }
    CancelIoEx(pipe, &overlapped);
    return false;
  }

  bool connectPipe(HANDLE pipe) {
    UniqueWindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    bool connected = false;
    if (ConnectNamedPipe(pipe, &overlapped)) {
      connected = true;
    } else {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        DWORD transferred = 0;
        connected = waitForPipeIo(pipe, overlapped, transferred);
      } else {
        connected = error == ERROR_PIPE_CONNECTED;
      }
    }

    return connected && !stopRequested.load(std::memory_order_acquire);
  }

  bool readExact(HANDLE pipe, void* data, DWORD size) {
    UniqueWindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      return false;
    }

    auto* cursor = static_cast<char*>(data);
    DWORD remaining = size;
    while (remaining > 0 && !stopRequested.load(std::memory_order_acquire)) {
      ResetEvent(event.get());
      OVERLAPPED overlapped{};
      overlapped.hEvent = event.get();
      DWORD transferred = 0;
      bool ok = false;
      if (ReadFile(pipe, cursor, remaining, nullptr, &overlapped)) {
        ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) !=
             FALSE;
      } else if (GetLastError() == ERROR_IO_PENDING) {
        ok = waitForPipeIo(pipe, overlapped, transferred);
      }

      if (!ok || transferred == 0) {
        return false;
      }
      cursor += transferred;
      remaining -= transferred;
    }
    return remaining == 0;
  }

  void readRequest(HANDLE pipe) {
    uint32_t flags = 0;
    if (!readExact(pipe, &flags, sizeof(flags)) ||
        (flags & ~kKnownShellOpenFlags) != 0) {
      return;
    }

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

    OpenFilesRequest request;
    request.files.push_back(std::move(file));
    request.videoMode = (flags & kShellOpenNonAsciiVideo) != 0
                            ? OpenVideoMode::Framebuffer
                            : OpenVideoMode::Ascii;
    requests.post(std::move(request));
  }

  ShellOpenSingleInstanceLock singleInstanceLock;
  UniqueWindowsHandle stopEvent;
  bool started = false;
  std::atomic<bool> stopRequested{false};
  std::thread worker;
  OpenFileRequests& requests;
};

WindowsShellOpenServer::WindowsShellOpenServer(OpenFileRequests& requests)
    : impl_(std::make_unique<Impl>(requests)) {}

WindowsShellOpenServer::~WindowsShellOpenServer() = default;

bool WindowsShellOpenServer::isAcceptingHandoffs() const {
  return impl_->isAcceptingHandoffs();
}
