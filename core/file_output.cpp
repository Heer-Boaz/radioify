#include "core/file_output.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <utility>

#include "core/runtime_helpers.h"

namespace file_output {
namespace {

std::atomic<std::uint64_t> gTemporarySequence{0};

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::uint64_t processId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

bool reserveFile(const std::filesystem::path& path) {
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
#else
  const int file = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (file < 0) return false;
  close(file);
  return true;
#endif
}

std::filesystem::path reserveTemporarySibling(
    const std::filesystem::path& destination) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
    const std::uint64_t sequence =
        gTemporarySequence.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path candidate = destination;
    candidate += L".radioify-" + std::to_wstring(processId()) + L"-" +
                 std::to_wstring(ticks) + L"-" +
                 std::to_wstring(sequence) + L".tmp";
    if (reserveFile(candidate)) return candidate;
  }
  return {};
}

}  // namespace

std::filesystem::path uniqueSiblingPath(
    const std::filesystem::path& sourcePath, const std::wstring& suffix,
    const std::wstring& extension) {
  if (sourcePath.filename().empty() || suffix.empty() || extension.empty()) {
    return {};
  }
  const std::filesystem::path directory = sourcePath.parent_path();
  const std::wstring stem = sourcePath.stem().wstring();
  std::error_code error;
  for (std::uint32_t index = 1; index < 100000; ++index) {
    const std::wstring number =
        index == 1 ? std::wstring()
                   : L" (" + std::to_wstring(index) + L")";
    const std::filesystem::path candidate =
        directory / std::filesystem::path(stem + suffix + number + extension);
    const bool exists = std::filesystem::exists(candidate, error);
    if (!error && !exists && candidate != sourcePath) return candidate;
    error.clear();
  }
  return {};
}

std::optional<Transaction> Transaction::begin(
    const std::filesystem::path& destination, PublishMode mode,
    std::string* error) {
  if (error) error->clear();
  if (destination.empty() || destination.filename().empty()) {
    setError(error, "The output path is empty.");
    return std::nullopt;
  }
  const std::filesystem::path parent = destination.parent_path();
  if (!parent.empty()) {
    std::error_code directoryError;
    if (!std::filesystem::is_directory(parent, directoryError) ||
        directoryError) {
      setError(error, "The output directory does not exist: " +
                          toUtf8String(parent));
      return std::nullopt;
    }
  }
  if (mode == PublishMode::CreateNew) {
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError) || existsError) {
      setError(error, existsError
                          ? "Could not inspect the output destination: " +
                                existsError.message()
                          : "The output already exists: " +
                                toUtf8String(destination.filename()));
      return std::nullopt;
    }
  }
  const std::filesystem::path temporary =
      reserveTemporarySibling(destination);
  if (temporary.empty()) {
    setError(error, "Could not reserve temporary output storage.");
    return std::nullopt;
  }
  return Transaction(destination, temporary, mode);
}

Transaction::Transaction(std::filesystem::path destination,
                         std::filesystem::path temporary, PublishMode mode)
    : destinationPath_(std::move(destination)),
      temporaryPath_(std::move(temporary)),
      mode_(mode) {}

Transaction::Transaction(Transaction&& other) noexcept
    : destinationPath_(std::move(other.destinationPath_)),
      temporaryPath_(std::move(other.temporaryPath_)),
      mode_(other.mode_),
      published_(other.published_) {
  other.published_ = true;
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this == &other) return *this;
  discard();
  destinationPath_ = std::move(other.destinationPath_);
  temporaryPath_ = std::move(other.temporaryPath_);
  mode_ = other.mode_;
  published_ = other.published_;
  other.published_ = true;
  return *this;
}

Transaction::~Transaction() { discard(); }

bool Transaction::publish(std::string* error) {
  if (published_ || temporaryPath_.empty()) {
    setError(error, "The output transaction is no longer publishable.");
    return false;
  }
#ifdef _WIN32
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (mode_ == PublishMode::ReplaceExisting) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  if (!MoveFileExW(temporaryPath_.c_str(), destinationPath_.c_str(), flags)) {
    setError(error, "Could not publish the output (Windows error " +
                        std::to_string(GetLastError()) + ").");
    return false;
  }
#else
  if (mode_ == PublishMode::CreateNew) {
    std::error_code existsError;
    if (std::filesystem::exists(destinationPath_, existsError) ||
        existsError) {
      setError(error, existsError ? existsError.message()
                                  : "The output already exists.");
      return false;
    }
  }
  std::error_code renameError;
  std::filesystem::rename(temporaryPath_, destinationPath_, renameError);
  if (renameError) {
    setError(error, "Could not publish the output: " +
                        renameError.message());
    return false;
  }
#endif
  published_ = true;
  temporaryPath_.clear();
  return true;
}

void Transaction::discard() {
  if (published_ || temporaryPath_.empty()) return;
  std::error_code ignored;
  std::filesystem::remove(temporaryPath_, ignored);
  temporaryPath_.clear();
}

}  // namespace file_output
