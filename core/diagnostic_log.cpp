#include "core/diagnostic_log.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include "core/runtime_helpers.h"
#include "core/timing_log.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::string safeFileComponent(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_') {
      result.push_back(static_cast<char>(character));
    } else if (!result.empty() && result.back() != '-') {
      result.push_back('-');
    }
  }
  while (!result.empty() && result.back() == '-') result.pop_back();
  return result.empty() ? "media-task" : result;
}

std::string filenameTimestamp() {
  using namespace std::chrono;
  const system_clock::time_point now = system_clock::now();
  const auto millisecondsPart =
      duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t time = system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer),
                "%04d%02d%02d-%02d%02d%02d-%03d",
                local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(millisecondsPart.count()));
  return buffer;
}

unsigned long processId() {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

const char* levelName(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::Info:
      return "INFO";
    case DiagnosticLevel::Warning:
      return "WARN";
    case DiagnosticLevel::Error:
      return "ERROR";
  }
  return "INFO";
}

}  // namespace

struct DiagnosticLog::Impl {
  explicit Impl(const std::filesystem::path& path)
      : stream(path, std::ios::binary | std::ios::trunc) {}

  std::mutex mutex;
  std::ofstream stream;
};

DiagnosticLog::DiagnosticLog(std::filesystem::path path)
    : path_(std::move(path)), impl_(std::make_unique<Impl>(path_)) {}

DiagnosticLog::~DiagnosticLog() = default;

void DiagnosticLog::append(DiagnosticLevel level,
                           std::string_view component,
                           std::string_view message) noexcept {
  if (!impl_) return;
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->stream) return;

    std::size_t offset = 0;
    do {
      const std::size_t newline = message.find('\n', offset);
      std::string_view line =
          newline == std::string_view::npos
              ? message.substr(offset)
              : message.substr(offset, newline - offset);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      impl_->stream << radioifyLogTimestamp() << " " << levelName(level)
                    << " [" << component << "] " << line << '\n';
      if (newline == std::string_view::npos) break;
      offset = newline + 1;
    } while (offset <= message.size());
    impl_->stream.flush();
  } catch (...) {
    // Diagnostics must never destabilize the operation they are observing.
  }
}

std::shared_ptr<DiagnosticLog> createDiagnosticLog(
    std::string_view operationName, std::string* error) {
  if (error) error->clear();
  try {
    const std::filesystem::path directory =
        radioifyWritableDataDir() / "logs" / "media-processing";
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
      if (error) {
        *error = "Could not create the media-processing log directory: " +
                 directoryError.message();
      }
      return {};
    }

    static std::atomic<unsigned long> sequence{0};
    const std::string filename =
        safeFileComponent(operationName) + "-" + filenameTimestamp() + "-" +
        std::to_string(processId()) + "-" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
        ".log";
    auto log = std::shared_ptr<DiagnosticLog>(
        new DiagnosticLog(directory / filename));
    if (!log->impl_ || !log->impl_->stream) {
      if (error) {
        *error = "Could not create the media-processing diagnostic log: " +
                 toUtf8String(log->path());
      }
      return {};
    }
    return log;
  } catch (const std::exception& exception) {
    if (error) {
      *error = std::string("Could not create the media-processing log: ") +
               exception.what();
    }
    return {};
  } catch (...) {
    if (error) *error = "Could not create the media-processing log.";
    return {};
  }
}
