#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace file_output {

enum class PublishMode {
  CreateNew,
  ReplaceExisting,
};

// Resolves a human-readable sibling path without silently overwriting an
// existing export. Publication still enforces CreateNew, so a concurrent
// writer cannot turn this convenience lookup into an overwrite.
std::filesystem::path uniqueSiblingPath(
    const std::filesystem::path& sourcePath, const std::wstring& suffix,
    const std::wstring& extension);

// Owns one same-directory temporary file and publishes it as a single
// filesystem transaction. Unpublished temporary data is removed on every
// exit path, including cancellation and exceptions.
class Transaction {
 public:
  static std::optional<Transaction> begin(
      const std::filesystem::path& destination, PublishMode mode,
      std::string* error);

  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  const std::filesystem::path& temporaryPath() const {
    return temporaryPath_;
  }
  const std::filesystem::path& destinationPath() const {
    return destinationPath_;
  }

  bool publish(std::string* error);

 private:
  Transaction(std::filesystem::path destination,
              std::filesystem::path temporary, PublishMode mode);
  void discard();

  std::filesystem::path destinationPath_;
  std::filesystem::path temporaryPath_;
  PublishMode mode_ = PublishMode::CreateNew;
  bool published_ = false;
};

struct TransactionDestination {
  std::filesystem::path path;
  PublishMode mode = PublishMode::CreateNew;
};

// Publishes a related set of sibling artifacts as one recoverable commit.
// Every producer writes to same-directory staging files first. If any rename
// fails, already-published members are removed and replaced destinations are
// restored from private backups before control returns to the caller.
class TransactionGroup {
 public:
  static std::optional<TransactionGroup> begin(
      std::vector<TransactionDestination> destinations,
      std::string* error);

  TransactionGroup(TransactionGroup&& other) noexcept;
  TransactionGroup& operator=(TransactionGroup&& other) noexcept;
  ~TransactionGroup();

  TransactionGroup(const TransactionGroup&) = delete;
  TransactionGroup& operator=(const TransactionGroup&) = delete;

  std::size_t size() const { return entries_.size(); }
  const std::filesystem::path& temporaryPath(std::size_t index) const;
  const std::filesystem::path& destinationPath(std::size_t index) const;
  const std::filesystem::path* temporaryPathFor(
      const std::filesystem::path& destination) const;

  bool publish(std::string* error);

 private:
  struct Entry {
    TransactionDestination destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool backupActive = false;
    bool stagedPublished = false;
  };

  explicit TransactionGroup(std::vector<Entry> entries)
      : entries_(std::move(entries)) {}
  bool rollback(std::string* detail);
  void discard();

  std::vector<Entry> entries_;
  bool published_ = false;
};

}  // namespace file_output
