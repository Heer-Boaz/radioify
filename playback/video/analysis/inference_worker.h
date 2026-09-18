#pragma once

#include <filesystem>
#include <string>

#include "playback/video/analysis/inference.h"
#include "playback/video/transcript/worker_types.h"

namespace playback_video_analysis {

// Shared by worker-backed jobs and in-process diagnostics. Acquire and destroy
// on the same thread, as required by the Windows mutex ownership contract.
class InferenceGpuLease final {
public:
  InferenceGpuLease() = default;
  ~InferenceGpuLease();
  InferenceGpuLease(const InferenceGpuLease &) = delete;
  InferenceGpuLease &operator=(const InferenceGpuLease &) = delete;
  OperationStatus acquire(const OperationControl &control, std::string *error);

private:
  void *mutex_ = nullptr;
};

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
playback_video_analysis::ReviewResult
runVideoReviewWorker(const playback_video_analysis::ReviewRequest &request,
                     const OperationControl &control,
                     const InferenceWorkspaceLease &lease);

playback_video_transcript::SpeechWorkerResult runSpeechTranscriptionWorker(
    const playback_video_transcript::SpeechWorkerRequest &request,
    const OperationControl &control, const InferenceWorkspaceLease &lease);

// Removes only the known private files for an exact source identity. Final
// review documents are owned by edit_review_store.cpp and are never touched
// here.
void discardInferenceWorkerWorkspace(const InferenceWorkspaceLease &lease);
void discardInferenceWorkerWorkspace(const std::string &sourceKey);

// Private helper-process entry point. This is intentionally not a user-facing
// CLI protocol; radioify.exe launches it with one owner-created workspace.
int inferenceWorkerMain(const std::filesystem::path &workspace);

} // namespace playback_video_analysis
