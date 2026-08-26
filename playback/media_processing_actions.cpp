#include "playback/media_processing_actions.h"

namespace playback_media_processing {

bool Actions::busy() const { return anyRunning && anyRunning(); }

bool Actions::requestSubtitles(
    const std::filesystem::path& sourceFile) const {
  return startSubtitles && startSubtitles(sourceFile);
}

bool Actions::requestSubtitleCancellation() const {
  return cancelSubtitles && cancelSubtitles();
}

bool Actions::requestAudioSeparation(
    const std::filesystem::path& sourceFile) const {
  return startAudioSeparation && startAudioSeparation(sourceFile);
}

bool Actions::requestAudioSeparationCancellation() const {
  return cancelAudioSeparation && cancelAudioSeparation();
}

void Actions::applySourceState(
    const std::filesystem::path& sourceFile,
    playback_media_actions::Context* context) const {
  if (!context) return;
  context->backgroundTaskRunning = busy();
  context->subtitleGenerationRunningForSource =
      subtitlesRunningFor && subtitlesRunningFor(sourceFile);
  context->canSeparateAudio =
      canSeparateAudio && canSeparateAudio(sourceFile);
  context->audioSeparationRunningForSource =
      audioSeparationRunningFor && audioSeparationRunningFor(sourceFile);
  context->hasSeparatedAudio =
      hasSeparatedAudio && hasSeparatedAudio(sourceFile);
}

}  // namespace playback_media_processing
