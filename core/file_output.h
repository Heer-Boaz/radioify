#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

}  // namespace file_output
