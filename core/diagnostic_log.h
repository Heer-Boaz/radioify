#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

enum class DiagnosticLevel {
  Info,
  Warning,
  Error,
};

// Append-only, crash-resilient diagnostics for one long-running operation.
// The object is safe to call from backend-owned worker threads and logging
// callbacks. It deliberately never writes to stdout/stderr because those
// streams may be owned by the live TUI.
class DiagnosticLog final {
 public:
  ~DiagnosticLog();

  DiagnosticLog(const DiagnosticLog&) = delete;
  DiagnosticLog& operator=(const DiagnosticLog&) = delete;

  const std::filesystem::path& path() const { return path_; }
  void append(DiagnosticLevel level, std::string_view component,
              std::string_view message) noexcept;

 private:
  friend std::shared_ptr<DiagnosticLog> createDiagnosticLog(
      std::string_view operationName, std::string* error);

  explicit DiagnosticLog(std::filesystem::path path);

  struct Impl;
  std::filesystem::path path_;
  std::unique_ptr<Impl> impl_;
};

// Creates a unique log below Radioify's writable data directory. Logging is
// supporting infrastructure: callers may continue without a log when this
// returns null, while preserving the creation error for user-facing details.
std::shared_ptr<DiagnosticLog> createDiagnosticLog(
    std::string_view operationName, std::string* error = nullptr);
