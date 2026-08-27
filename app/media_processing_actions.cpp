#include "app/media_processing_actions.h"

#include <cstdint>

#include "app/media_processing_coordinator.h"
#include "audio/analysis/melody_artifact_paths.h"
#include "audio/loopsplit/output_paths.h"

namespace media_processing {
namespace {

bool validTrackIndex(const std::optional<int>& trackIndex) {
  return !trackIndex || *trackIndex >= 0;
}

}  // namespace

Actions::Actions(Coordinator& coordinator)
    : coordinator_(coordinator), sourceActions_(coordinator) {}

std::optional<playback_media_processing::ActionResult> Actions::execute(
    const ActionRequest& request) const {
  if (std::optional<playback_media_processing::ActionResult> sourceResult =
          sourceActions_.execute(request.action, request.sourceFile)) {
    return sourceResult;
  }

  switch (request.action) {
    case playback_media_actions::Action::AnalyzeAudio: {
      if (!validTrackIndex(request.trackIndex)) {
        return playback_media_processing::makeActionResult(
            request.action, request.sourceFile,
            playback_media_processing::RequestResult::rejected(
                playback_media_processing::RequestFailure::InvalidSelection));
      }
      const std::filesystem::path outputFile =
          request.trackIndex
              ? melodyArtifactPathForTrack(
                    request.sourceFile,
                    static_cast<std::uint32_t>(*request.trackIndex))
              : defaultMelodyArtifactPath(request.sourceFile);
      return playback_media_processing::makeActionResult(
          request.action, request.sourceFile,
          coordinator_.tryStartMelodyAnalysis(
              request.sourceFile, request.trackIndex.value_or(0), outputFile));
    }
    case playback_media_actions::Action::SplitLoop: {
      if (!validTrackIndex(request.trackIndex)) {
        return playback_media_processing::makeActionResult(
            request.action, request.sourceFile,
            playback_media_processing::RequestResult::rejected(
                playback_media_processing::RequestFailure::InvalidSelection));
      }
      LoopSplitConfig config = request.loopSplitConfig;
      config.trackIndex = request.trackIndex.value_or(0);
      const LoopSplitOutputPaths outputPaths = resolveLoopSplitOutputPaths(
          request.sourceFile, request.outputArgument);
      return playback_media_processing::makeActionResult(
          request.action, request.sourceFile,
          coordinator_.tryStartLoopSplit(request.sourceFile,
                                         outputPaths.stinger,
                                         outputPaths.loop, config));
    }
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
      return std::nullopt;
    case playback_media_actions::Action::GenerateSubtitles:
    case playback_media_actions::Action::CancelSubtitleGeneration:
    case playback_media_actions::Action::ExportTranscriptText:
    case playback_media_actions::Action::ExportAudio:
    case playback_media_actions::Action::CancelMediaExport:
    case playback_media_actions::Action::SeparateAudio:
    case playback_media_actions::Action::CancelAudioSeparation:
      break;
  }
  return std::nullopt;
}

playback_media_actions::Context Actions::contextForSource(
    const std::filesystem::path& sourceFile) const {
  return sourceActions_.contextForSource(sourceFile);
}

}  // namespace media_processing
