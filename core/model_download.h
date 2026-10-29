#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace model_download {

struct Artifact {
  const wchar_t *host;
  const wchar_t *objectPath;
  const char *label;
  std::uintmax_t bytes;
  const char *sha256;
};

struct Control {
  std::function<bool()> cancelled;
  std::function<void(double, const std::string &)> progress;
};

enum class Status { Succeeded, Cancelled, Failed };
struct Result {
  Status status = Status::Failed;
  std::string detail;
};

// No network access. Verified receipts avoid rehashing unchanged large models.
bool verified(const std::filesystem::path &path, const Artifact &artifact,
              const Control &control = {}, std::string *error = nullptr);

// HTTPS, resumable staging, per-model process lock, SHA-256 before publication.
// A valid cached model is usable offline. Cancellation retains the partial
// file.
Result ensure(const std::filesystem::path &destination,
              const Artifact &artifact, const Control &control = {});

} // namespace model_download
