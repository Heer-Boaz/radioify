#pragma once

#include <filesystem>
#include <string>

#include "playback/video/chapter/inference.h"

namespace playback_video_chapters {

struct InferenceWorkspaceLeaseResult;

// Exclusive interprocess ownership of one source-bound inference workspace.
// The lease deliberately outlives the child process: callers retain it while
// validating and committing the final cache entry so another Radioify process
// can never resume or remove the same checkpoint concurrently.
class InferenceWorkspaceLease final {
public:
  InferenceWorkspaceLease() = default;
  ~InferenceWorkspaceLease();

  InferenceWorkspaceLease(const InferenceWorkspaceLease &) = delete;
  InferenceWorkspaceLease &operator=(const InferenceWorkspaceLease &) = delete;
  InferenceWorkspaceLease(InferenceWorkspaceLease &&other) noexcept;
  InferenceWorkspaceLease &operator=(InferenceWorkspaceLease &&other) noexcept;

  explicit operator bool() const { return mutex_ != nullptr; }
  const std::string &sourceKey() const { return sourceKey_; }

private:
  friend struct InferenceWorkspaceLeaseResult;
  friend InferenceWorkspaceLeaseResult
  acquireInferenceWorkspaceLease(const std::string &, const OperationControl &);

  InferenceWorkspaceLease(void *mutex, std::string sourceKey);
  void reset() noexcept;

  void *mutex_ = nullptr;
  std::string sourceKey_;
};

struct InferenceWorkspaceLeaseResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  InferenceWorkspaceLease lease;
};

InferenceWorkspaceLeaseResult
acquireInferenceWorkspaceLease(const std::string &sourceKey,
                               const OperationControl &control);

// Executes model inference in a killable child process. The parent retains
// ownership of scheduling and can therefore reclaim Vulkan deterministically
// even while a third-party GPU dispatch is not cooperatively interruptible.
InferenceResult runInferenceWorker(const InferenceRequest &request,
                                   const OperationControl &control,
                                   const InferenceWorkspaceLease &lease);

// Runs the published ASR-only chapter-planning stage in the same killable
// worker boundary as visual metadata inference. Its typed boundaries and
// titles cross back to the parent; native model state remains child-owned.
SpeechChapterPlanResult runSpeechChapterPlanWorker(
    const SpeechChapterPlanRequest &request,
    const OperationControl &control,
    const InferenceWorkspaceLease &lease);

// Removes only the known private files for an exact source identity. Final
// chapter documents are owned by cache.cpp and are never touched here.
void discardInferenceWorkerWorkspace(const InferenceWorkspaceLease &lease);
void discardInferenceWorkerWorkspace(const std::string &sourceKey);
void discardInferenceWorkerWorkspaceIfIdle(const std::string &sourceKey);

// Private helper-process entry point. This is intentionally not a user-facing
// CLI protocol; radioify.exe launches it with one owner-created workspace.
int inferenceWorkerMain(const std::filesystem::path &workspace);

} // namespace playback_video_chapters
