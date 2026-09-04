#include "app/chapter_analysis_cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/runtime_helpers.h"
#include "playback/video/chapter/cache.h"
#include "playback/video/chapter/service.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/decoder.h"
#include "playback/video/subtitle/manager.h"

namespace {

using playback_video_chapters::AnalysisState;
using playback_video_chapters::Snapshot;

const char* stateName(AnalysisState state) {
  switch (state) {
    case AnalysisState::Disabled:
      return "disabled";
    case AnalysisState::CheckingSupport:
      return "checking-support";
    case AnalysisState::SetupRequired:
      return "setup-required";
    case AnalysisState::Installing:
      return "installing";
    case AnalysisState::WaitingForPlayback:
      return "waiting-for-gpu";
    case AnalysisState::Analyzing:
      return "analyzing";
    case AnalysisState::Ready:
      return "ready";
    case AnalysisState::Unsupported:
      return "unsupported";
    case AnalysisState::Failed:
      return "failed";
  }
  return "unknown";
}

bool terminal(AnalysisState state) {
  return state == AnalysisState::Disabled ||
         state == AnalysisState::SetupRequired ||
         state == AnalysisState::Ready || state == AnalysisState::Unsupported ||
         state == AnalysisState::Failed;
}

int exitCode(AnalysisState state) {
  switch (state) {
    case AnalysisState::Ready:
      return 0;
    case AnalysisState::SetupRequired:
      return 2;
    case AnalysisState::Unsupported:
      return 3;
    default:
      return 4;
  }
}

void publishDocument(const std::filesystem::path& file,
                     const playback_video_chapters::AnalysisRequest* request,
                     const Snapshot& snapshot, bool cacheHit,
                     bool hasEnglishText) {
  nlohmann::json document = {
      {"schema", 1},
      {"state", stateName(snapshot.state)},
      {"source", toUtf8String(file)},
      {"duration_us", snapshot.durationUs},
      {"cache_hit", cacheHit},
      {"english_text_evidence", hasEnglishText},
      {"detail", snapshot.detail},
      {"warning", snapshot.warning},
  };
  if (request) {
    const std::filesystem::path cachePath =
        playback_video_chapters::analysisCachePath(*request);
    std::error_code error;
    const bool persisted = std::filesystem::is_regular_file(cachePath, error) &&
                           !error;
    document["cache_key"] =
        playback_video_chapters::analysisSourceKey(*request);
    document["cache_path"] = toUtf8String(cachePath);
    document["persisted"] = persisted;
  }
  nlohmann::json chapters = nlohmann::json::array();
  for (const playback_video_chapters::Chapter& chapter : snapshot.chapters) {
    chapters.push_back({{"id", chapter.id},
                        {"start_us", chapter.startUs},
                        {"end_us", chapter.endUs},
                        {"title", chapter.title}});
  }
  document["chapters"] = std::move(chapters);
  std::cout << document.dump(2) << '\n';
}

void reportProgress(const Snapshot& snapshot) {
  std::cerr << "chapter-analysis state=" << stateName(snapshot.state);
  if (snapshot.progress) {
    std::cerr << " progress=" << std::fixed << std::setprecision(3)
              << *snapshot.progress;
  }
  if (!snapshot.phase.empty())
    std::cerr << " phase=" << snapshot.phase;
  std::cerr << '\n';
}

bool progressMilestone(const Snapshot& snapshot,
                       std::optional<AnalysisState> previousState,
                       const std::string& previousPhase,
                       int previousPercent) {
  if (!previousState || snapshot.state != *previousState ||
      snapshot.phase != previousPhase || terminal(snapshot.state)) {
    return true;
  }
  const int percent = snapshot.progress
                          ? static_cast<int>(std::floor(
                                std::clamp(*snapshot.progress, 0.0, 1.0) * 100.0))
                          : -1;
  return percent != previousPercent;
}

}  // namespace

int runChapterAnalysisCli(const std::filesystem::path& file) {
  try {
    VideoMetadata metadata;
    std::string probeError;
    if (!probeVideoMetadata(file, &metadata, &probeError) ||
        metadata.videoStreamIndex < 0 || metadata.duration100ns <= 0) {
      Snapshot failed;
      failed.state = AnalysisState::Unsupported;
      failed.detail = probeError.empty()
                          ? "The input has no seekable video stream."
                          : std::move(probeError);
      publishDocument(file, nullptr, failed, false, false);
      return exitCode(failed.state);
    }

    SubtitleManager subtitles;
    subtitles.loadForVideo(file);

    playback_video_chapters::AnalysisRequest request;
    request.file = file;
    request.videoStreamIndex = metadata.videoStreamIndex;
    request.durationUs = metadata.duration100ns / 10;
    request.sourceWidth = metadata.width;
    request.sourceHeight = metadata.height;
    request.englishText =
        playback_video_chapters::selectEnglishTextEvidence(subtitles, file);

    const bool cacheHit =
        playback_video_chapters::loadCachedAnalysis(request).has_value();
    const bool hasEnglishText = request.englishText.has_value();
    playback_video_chapters::Service service;
    const playback_video_chapters::Service::RequestId requestId =
        service.start(request);
    service.setBackgroundGpuAllowed(requestId, true);

    Snapshot snapshot = service.snapshot(requestId);
    std::optional<AnalysisState> reportedState;
    std::string reportedPhase;
    int reportedPercent = -1;
    for (;;) {
      if (progressMilestone(snapshot, reportedState, reportedPhase,
                            reportedPercent)) {
        reportProgress(snapshot);
        reportedState = snapshot.state;
        reportedPhase = snapshot.phase;
        reportedPercent = snapshot.progress
                              ? static_cast<int>(std::floor(std::clamp(
                                    *snapshot.progress, 0.0, 1.0) * 100.0))
                              : -1;
      }
      if (terminal(snapshot.state))
        break;

      const NativeWaitHandle changed = service.changedWaitHandle();
      if (!changed) {
        snapshot.state = AnalysisState::Failed;
        snapshot.detail = "The chapter service has no completion signal.";
        break;
      }
      const DWORD wait =
          WaitForSingleObject(static_cast<HANDLE>(changed.get()), 30'000);
      if (wait == WAIT_FAILED) {
        snapshot.state = AnalysisState::Failed;
        snapshot.detail = "Waiting for chapter analysis failed.";
        break;
      }
      if (wait == WAIT_TIMEOUT) {
        std::cerr << "chapter-analysis waiting state="
                  << stateName(snapshot.state) << '\n';
        continue;
      }
      service.consumeChanged();
      snapshot = service.snapshot(requestId);
    }

    publishDocument(file, &request, snapshot, cacheHit, hasEnglishText);
    service.cancel(requestId);
    return exitCode(snapshot.state);
  } catch (const std::exception& error) {
    Snapshot failed;
    failed.state = AnalysisState::Failed;
    failed.detail = std::string("Chapter analysis failed: ") + error.what();
    publishDocument(file, nullptr, failed, false, false);
    return exitCode(failed.state);
  } catch (...) {
    Snapshot failed;
    failed.state = AnalysisState::Failed;
    failed.detail = "Chapter analysis failed unexpectedly.";
    publishDocument(file, nullptr, failed, false, false);
    return exitCode(failed.state);
  }
}
