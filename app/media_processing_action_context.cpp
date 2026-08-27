#include "app/media_processing_actions.h"

#include "audio/audioplayback.h"

namespace media_processing {

ActionRequest captureActionRequest(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile,
    std::optional<int> trackIndex, const std::string& outputArgument,
    const AudioPlaybackRuntime& audioPlayback) {
  ActionRequest request;
  request.action = action;
  request.sourceFile = sourceFile;
  request.trackIndex = trackIndex;
  request.outputArgument = outputArgument;
  if (action == playback_media_actions::Action::SplitLoop) {
    request.loopSplitConfig.trackIndex = trackIndex.value_or(0);
    request.loopSplitConfig.kssOptions = audioPlayback.kssOptions();
    request.loopSplitConfig.nsfOptions = audioPlayback.nsfOptions();
    request.loopSplitConfig.vgmOptions = audioPlayback.vgmOptions();
  }
  return request;
}

}  // namespace media_processing
