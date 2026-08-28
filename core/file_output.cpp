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
#include <cwctype>
#include <cstdint>
#include <system_error>
#include <unordered_set>
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

std::wstring destinationIdentity(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path identity =
      std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    identity = std::filesystem::absolute(path, error);
  }
  if (error) identity = path;
  std::wstring key = identity.lexically_normal().wstring();
#ifdef _WIN32
  for (wchar_t& character : key) {
    character = static_cast<wchar_t>(std::towlower(character));
  }
#endif
  return key;
}

bool movePath(const std::filesystem::path& source,
              const std::filesystem::path& destination, bool replace,
              std::string* error) {
#ifdef _WIN32
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (replace) flags |= MOVEFILE_REPLACE_EXISTING;
  if (MoveFileExW(source.c_str(), destination.c_str(), flags)) return true;
  setError(error, "Windows error " + std::to_string(GetLastError()));
  return false;
#else
  if (!replace) {
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError) || existsError) {
      setError(error, existsError ? existsError.message()
                                  : "the destination already exists");
      return false;
    }
  }
  std::error_code moveError;
  std::filesystem::rename(source, destination, moveError);
  if (!moveError) return true;
  setError(error, moveError.message());
  return false;
#endif
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

std::optional<TransactionGroup> TransactionGroup::begin(
    std::vector<TransactionDestination> destinations,
    std::string* error) {
  if (error) error->clear();
  if (destinations.empty()) {
    setError(error, "The output transaction has no destinations.");
    return std::nullopt;
  }

  std::unordered_set<std::wstring> identities;
  TransactionGroup group({});
  group.entries_.reserve(destinations.size());
  for (TransactionDestination& destination : destinations) {
    if (destination.path.empty() || destination.path.filename().empty()) {
      setError(error, "An output path is empty.");
      return std::nullopt;
    }
    if (!identities.insert(destinationIdentity(destination.path)).second) {
      setError(error, "The output transaction contains the same destination "
                      "more than once.");
      return std::nullopt;
    }
    const std::filesystem::path parent = destination.path.parent_path();
    if (!parent.empty()) {
      std::error_code directoryError;
      if (!std::filesystem::is_directory(parent, directoryError) ||
          directoryError) {
        setError(error, "The output directory does not exist: " +
                            toUtf8String(parent));
        return std::nullopt;
      }
    }
    if (destination.mode == PublishMode::CreateNew) {
      std::error_code existsError;
      if (std::filesystem::exists(destination.path, existsError) ||
          existsError) {
        setError(error, existsError
                            ? "Could not inspect an output destination: " +
                                  existsError.message()
                            : "An output already exists: " +
                                  toUtf8String(destination.path.filename()));
        return std::nullopt;
      }
    }
    std::filesystem::path temporary =
        reserveTemporarySibling(destination.path);
    if (temporary.empty()) {
      setError(error, "Could not reserve temporary output storage.");
      return std::nullopt;
    }
    group.entries_.push_back(
        Entry{std::move(destination), std::move(temporary), {}, false, false});
  }
  return group;
}

TransactionGroup::TransactionGroup(TransactionGroup&& other) noexcept
    : entries_(std::move(other.entries_)), published_(other.published_) {
  other.published_ = true;
}

TransactionGroup& TransactionGroup::operator=(
    TransactionGroup&& other) noexcept {
  if (this == &other) return *this;
  discard();
  entries_ = std::move(other.entries_);
  published_ = other.published_;
  other.published_ = true;
  return *this;
}

TransactionGroup::~TransactionGroup() { discard(); }

const std::filesystem::path& TransactionGroup::temporaryPath(
    std::size_t index) const {
  return entries_.at(index).temporary;
}

const std::filesystem::path& TransactionGroup::destinationPath(
    std::size_t index) const {
  return entries_.at(index).destination.path;
}

const std::filesystem::path* TransactionGroup::temporaryPathFor(
    const std::filesystem::path& destination) const {
  const std::wstring identity = destinationIdentity(destination);
  for (const Entry& entry : entries_) {
    if (destinationIdentity(entry.destination.path) == identity) {
      return &entry.temporary;
    }
  }
  return nullptr;
}

bool TransactionGroup::rollback(std::string* detail) {
  bool restored = true;
  std::string rollbackError;
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    if (entry->stagedPublished) {
      std::error_code removeError;
      std::filesystem::remove(entry->destination.path, removeError);
      if (removeError) {
        restored = false;
        if (rollbackError.empty()) rollbackError = removeError.message();
      } else {
        entry->stagedPublished = false;
      }
    }
    if (entry->backupActive) {
      std::string moveError;
      if (movePath(entry->backup, entry->destination.path, true,
                   &moveError)) {
        entry->stagedPublished = false;
        entry->backupActive = false;
        entry->backup.clear();
      } else {
        restored = false;
        if (rollbackError.empty()) rollbackError = std::move(moveError);
      }
    }
  }
  if (!restored && detail) {
    *detail = "Rollback could not fully restore the previous outputs (" +
              rollbackError + ").";
  }
  return restored;
}

bool TransactionGroup::publish(std::string* error) {
  if (published_ || entries_.empty()) {
    setError(error, "The output transaction is no longer publishable.");
    return false;
  }

  for (Entry& entry : entries_) {
    if (entry.destination.mode != PublishMode::ReplaceExisting) continue;
    std::error_code existsError;
    const bool exists =
        std::filesystem::exists(entry.destination.path, existsError);
    if (existsError) {
      setError(error, "Could not inspect an output destination: " +
                          existsError.message());
      std::string ignored;
      rollback(&ignored);
      return false;
    }
    if (!exists) continue;
    entry.backup = reserveTemporarySibling(entry.destination.path);
    if (entry.backup.empty()) {
      setError(error, "Could not reserve output rollback storage.");
      std::string ignored;
      rollback(&ignored);
      return false;
    }
    std::string moveError;
    if (!movePath(entry.destination.path, entry.backup, true, &moveError)) {
      setError(error, "Could not prepare the output commit (" + moveError +
                          ").");
      std::error_code ignoredRemove;
      std::filesystem::remove(entry.backup, ignoredRemove);
      entry.backup.clear();
      std::string ignored;
      rollback(&ignored);
      return false;
    }
    entry.backupActive = true;
  }

  for (Entry& entry : entries_) {
    std::string moveError;
    if (!movePath(entry.temporary, entry.destination.path, false,
                  &moveError)) {
      const std::string primary =
          "Could not publish all outputs (" + moveError + ").";
      std::string rollbackDetail;
      const bool restored = rollback(&rollbackDetail);
      setError(error, restored ? primary
                               : primary + " " + rollbackDetail);
      return false;
    }
    entry.temporary.clear();
    entry.stagedPublished = true;
  }

  for (Entry& entry : entries_) {
    if (entry.backupActive) {
      std::error_code ignored;
      std::filesystem::remove(entry.backup, ignored);
      entry.backup.clear();
      entry.backupActive = false;
    }
    entry.stagedPublished = false;
  }
  published_ = true;
  return true;
}

void TransactionGroup::discard() {
  if (published_) return;
  std::string ignoredRollback;
  rollback(&ignoredRollback);
  for (Entry& entry : entries_) {
    if (!entry.temporary.empty()) {
      std::error_code ignored;
      std::filesystem::remove(entry.temporary, ignored);
      entry.temporary.clear();
    }
    if (!entry.backupActive && !entry.backup.empty()) {
      std::error_code ignored;
      std::filesystem::remove(entry.backup, ignored);
      entry.backup.clear();
    }
  }
  entries_.clear();
}

}  // namespace file_output
