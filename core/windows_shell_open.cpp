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
#include <vector>

#include "open_file_requests.h"
#include "runtime_helpers.h"
#include "shell_open_protocol.h"
#include "shell_open_mode.h"
#include "windows_app_resources.h"
#include "windows_handle.h"

namespace {

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
  return RADIOIFY_APP_NAME_W L".ShellOpen.3." + std::to_wstring(sessionId);
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

std::uint64_t nextShellOpenRequestId() {
  static std::atomic<std::uint64_t> sequence{
      (GetTickCount64() << 16) ^
      static_cast<std::uint64_t>(GetCurrentProcessId())};
  std::uint64_t requestId =
      sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  if (requestId == 0) {
    requestId = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return requestId;
}

bool transactShellOpenRequest(const std::vector<std::uint8_t>& request,
                              std::vector<std::uint8_t>& response,
                              uint32_t timeoutMs) {
  const std::wstring pipeName = shellOpenPipeName();
  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);

  for (;;) {
    UniqueWindowsHandle pipe(CreateFileW(
        pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe) {
      DWORD readMode = PIPE_READMODE_MESSAGE;
      if (!SetNamedPipeHandleState(pipe.get(), &readMode, nullptr, nullptr)) {
        return false;
      }

      UniqueWindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!event) return false;

      response.assign(shell_open_protocol::kMaxMessageBytes, 0);
      OVERLAPPED overlapped{};
      overlapped.hEvent = event.get();
      bool pending = false;
      if (!TransactNamedPipe(
              pipe.get(), const_cast<std::uint8_t*>(request.data()),
              static_cast<DWORD>(request.size()), response.data(),
              static_cast<DWORD>(response.size()), nullptr, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        pending = true;
      }

      if (pending) {
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining =
            now >= deadline
                ? 0
                : static_cast<DWORD>((std::min)(
                      deadline - now,
                      static_cast<ULONGLONG>(MAXDWORD - 1)));
        if (WaitForSingleObject(event.get(), remaining) != WAIT_OBJECT_0) {
          CancelIoEx(pipe.get(), &overlapped);
          DWORD ignored = 0;
          GetOverlappedResult(pipe.get(), &overlapped, &ignored, TRUE);
          return false;
        }
      }

      DWORD received = 0;
      if (!GetOverlappedResult(pipe.get(), &overlapped, &received, FALSE) ||
          received == 0 || received > response.size()) {
        return false;
      }
      response.resize(received);
      return true;
    }

    const DWORD error = GetLastError();
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline ||
        (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)) {
      return false;
    }

    const DWORD waitMs = static_cast<DWORD>(
        (std::min)(deadline - now, static_cast<ULONGLONG>(100)));
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
                                 OpenPresentationDirective presentation,
                                 uint32_t timeoutMs) {
  shell_open_protocol::Request request;
  request.requestId = nextShellOpenRequestId();
  request.openFiles.files.push_back(file);
  request.openFiles.presentation = presentation;
  const std::optional<std::vector<std::uint8_t>> encoded =
      shell_open_protocol::encodeRequest(request);
  if (!encoded) return false;

  std::vector<std::uint8_t> responseBytes;
  if (!transactShellOpenRequest(*encoded, responseBytes, timeoutMs)) {
    return false;
  }
  shell_open_protocol::ResponseDecodeResult decoded =
      shell_open_protocol::decodeResponse(responseBytes);
  return decoded.value && decoded.value->requestId == request.requestId &&
         decoded.value->status == shell_open_protocol::ResponseStatus::Accepted;
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
          pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
          static_cast<DWORD>(shell_open_protocol::kMaxMessageBytes),
          static_cast<DWORD>(shell_open_protocol::kMaxMessageBytes), 0,
          nullptr));
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

  bool readMessage(HANDLE pipe, std::vector<std::uint8_t>& message) {
    UniqueWindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;

    message.assign(shell_open_protocol::kMaxMessageBytes, 0);
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    bool ok = false;
    if (ReadFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                 nullptr, &overlapped)) {
      ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) !=
           FALSE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
      ok = waitForPipeIo(pipe, overlapped, transferred);
    }
    if (!ok || transferred == 0 || transferred > message.size()) return false;
    message.resize(transferred);
    return true;
  }

  bool writeMessage(HANDLE pipe,
                    const std::vector<std::uint8_t>& message) {
    UniqueWindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    bool ok = false;
    if (WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                  nullptr, &overlapped)) {
      ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) !=
           FALSE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
      ok = waitForPipeIo(pipe, overlapped, transferred);
    }
    return ok && transferred == message.size();
  }

  void readRequest(HANDLE pipe) {
    std::vector<std::uint8_t> message;
    if (!readMessage(pipe, message)) return;

    shell_open_protocol::RequestDecodeResult decoded =
        shell_open_protocol::decodeRequest(message);
    shell_open_protocol::Response response;
    if (!decoded.value) {
      response.status =
          decoded.error == shell_open_protocol::DecodeError::UnsupportedVersion
              ? shell_open_protocol::ResponseStatus::UnsupportedVersion
              : shell_open_protocol::ResponseStatus::InvalidRequest;
    } else {
      response.requestId = decoded.value->requestId;
      try {
        requests.post(std::move(decoded.value->openFiles));
        response.status = shell_open_protocol::ResponseStatus::Accepted;
      } catch (...) {
        response.status = shell_open_protocol::ResponseStatus::InternalError;
      }
    }

    const std::vector<std::uint8_t> responseMessage =
        shell_open_protocol::encodeResponse(response);
    if (writeMessage(pipe, responseMessage)) {
      FlushFileBuffers(pipe);
    }
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
