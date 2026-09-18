#include "playback/video/analysis/edit_review_backend.h"

#include <chrono>
#include <thread>

#include "playback/video/analysis/edit_review_store.h"
#include "playback/video/analysis/evidence_preparation.h"
#include "playback/video/analysis/inference_worker.h"
#include "playback/video/analysis/model.h"

namespace playback_video_analysis {

bool reviewVideoForEditing(const JobRequest &job,
                           const AnalysisProgressCallback &progress,
                           const std::atomic<bool> *cancelled,
                           ReviewJobResult *result, std::string *error) {
  if (!result || !error)
    return false;
  ReviewRequest request;
  request.sourcePath = job.sourcePath;
  request.videoStreamIndex = job.videoStreamIndex;
  request.durationUs = job.durationUs;
  const auto model = resolveVideoReviewModelPaths();
  request.model = model.model;
  request.projector = model.projector;
  const auto windows = reviewWindows(request.durationUs);
  if (windows.empty()) {
    *error = "Could not identify the video for editing review.";
    return false;
  }
  double currentFraction = 0.0;
  OperationControl control;
  control.cancelled = [cancelled] { return cancelled && cancelled->load(); };
  control.progress = [&](std::optional<double> fraction, std::string phase) {
    if (fraction)
      currentFraction = *fraction;
    if (progress)
      progress({currentFraction, std::move(phase)});
  };
  InferenceEngine gpu;
  control.backgroundGpuAllowed = [&] {
    const auto budget = gpu.gpuMemoryBudget();
    return budget && budget->freeBytes >= 2ull * 1024 * 1024 * 1024;
  };
  if (!gpu.gpuMemoryBudget()) {
    *error = "Editing analysis needs driver-wide GPU memory telemetry (NVIDIA "
             "NVML).";
    return false;
  }
  EvidencePreparation speech;
  for (;;) {
    if (control.cancelled())
      return false;
    const auto prepared = speech.prepare(job, control);
    if (prepared.status == OperationStatus::Succeeded) {
      if (prepared.evidence)
        request.speech = prepared.evidence->cues;
      break;
    }
    if (prepared.status != OperationStatus::Yielded) {
      *error = prepared.detail;
      return false;
    }
    if (progress)
      progress({currentFraction, "Waiting for GPU memory for speech analysis"});
    for (int tick = 0; tick < 40 && !control.cancelled(); ++tick)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  request.identity = reviewIdentity(request);
  if (request.identity.empty()) {
    *error = "Could not identify the video for editing review.";
    return false;
  }
  control.progress = [&](std::optional<double> fraction, std::string phase) {
    if (fraction)
      currentFraction = 0.2 + 0.8 * *fraction;
    if (progress)
      progress({currentFraction, std::move(phase)});
  };
  const auto lease = acquireInferenceWorkspaceLease(request.identity, control);
  if (!lease.lease) {
    *error = lease.detail;
    return false;
  }
  if (job.forceReanalysis) {
    std::error_code ec;
    std::filesystem::remove(reviewCachePath(request), ec);
    if (ec) {
      *error = "Could not replace the previous video editing review.";
      return false;
    }
  }
  ReviewProgress completedReview;
  (void)loadReviewProgress(request, &completedReview);
  currentFraction =
      0.2 + 0.8 * reviewProgressFraction(request.durationUs, completedReview);
  if (!reviewComplete(request.durationUs, completedReview)) {
    const auto initialBudget = gpu.gpuMemoryBudget();
    if (!initialBudget) {
      *error = "Editing analysis needs driver-wide GPU memory telemetry; this "
               "GPU/driver is not supported (NVIDIA NVML required).";
      return false;
    }
    if (initialBudget->totalBytes < 11ull * 1024 * 1024 * 1024) {
      *error = "The video editing model needs at least 11 GiB of dedicated GPU "
               "memory.";
      return false;
    }
    const double completed =
        reviewProgressFraction(request.durationUs, completedReview);
    if (progress)
      progress(
          {0.2 + 0.8 * completed, "Preparing the local video editing model"});
    auto installationControl = control;
    installationControl.progress = [&](std::optional<double> fraction,
                                       std::string phase) {
      if (fraction)
        phase += " (" + std::to_string(int(*fraction * 100.0)) + "%)";
      if (progress)
        progress({0.2 + 0.8 * completed, std::move(phase)});
    };
    const auto installed = installVideoReviewModel(model, installationControl);
    if (installed.status != OperationStatus::Succeeded) {
      *error = installed.detail;
      return false;
    }
    // Keep a margin for playback and other applications. An explicit editing
    // job waits for memory; it never reduces the sampling rate to make room.
    control.backgroundGpuAllowed = [&] {
      const auto budget = gpu.gpuMemoryBudget();
      return budget && budget->freeBytes >= 2ull * 1024 * 1024 * 1024;
    };
    for (;;) {
      if (control.cancelled())
        return false;
      const auto budget = gpu.gpuMemoryBudget();
      if (!budget) {
        *error = "Editing analysis needs driver-wide GPU memory telemetry; "
                 "this GPU/driver is not supported (NVIDIA NVML required).";
        return false;
      }
      if (request.identity != reviewIdentity(request)) {
        *error = "The source video changed during editing review.";
        return false;
      }
      const auto reviewed = runVideoReviewWorker(request, control, lease.lease);
      if (reviewed.status == OperationStatus::Succeeded) {
        completedReview = reviewed.progress;
        break;
      }
      if (reviewed.status != OperationStatus::Yielded) {
        *error = reviewed.detail;
        return false;
      }
      (void)loadReviewProgress(request, &completedReview);
      currentFraction = 0.2 + 0.8 * reviewProgressFraction(request.durationUs,
                                                           completedReview);
      if (progress)
        progress({currentFraction,
                  "Waiting for GPU memory (11 GiB before loading)"});
      for (int tick = 0; tick < 40 && !control.cancelled(); ++tick)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (request.identity != reviewIdentity(request)) {
    *error = "The source changed; these editing proposals were not published.";
    return false;
  }
  if (control.cancelled())
    return false;
  result->suggestions =
      assembleEditProposals(request.durationUs, completedReview);
  result->visualSampleCount = 0;
  for (const auto &observation : completedReview.observations)
    result->visualSampleCount += observation.frameTimesUs.size();
  if (progress)
    progress(
        {1.0,
         request.speech.empty()
             ? "Editing proposals ready (video at 2 fps; no speech available)"
             : "Editing proposals ready (video at 2 fps and timed speech)"});
  return true;
}

} // namespace playback_video_analysis
