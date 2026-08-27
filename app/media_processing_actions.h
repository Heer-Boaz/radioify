#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "audio/loopsplit/loopsplit.h"
#include "playback/media_processing_actions.h"

class AudioPlaybackRuntime;

namespace media_processing {

class Coordinator;

struct ActionRequest {
  playback_media_actions::Action action =
      playback_media_actions::Action::Play;
  std::filesystem::path sourceFile;
  std::optional<int> trackIndex;
  std::string outputArgument;
  LoopSplitConfig loopSplitConfig;
};

// Captures the live audio-option state needed by an offline media action. The
// resulting request is a value and can safely outlive the browser selection
// that initiated it.
ActionRequest captureActionRequest(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile,
    std::optional<int> trackIndex, const std::string& outputArgument,
    const AudioPlaybackRuntime& audioPlayback);

// Application-level command facade for every background media action. It
// owns output naming and request validation while Coordinator owns worker
// lifetime, mutual exclusion, progress and cancellation.
class Actions {
 public:
  explicit Actions(Coordinator& coordinator);

  std::optional<playback_media_processing::ActionResult> execute(
      const ActionRequest& request) const;
  void applySourceState(const std::filesystem::path& sourceFile,
                        playback_media_actions::Context& context) const;
  playback_media_processing::Actions playbackActions() const {
    return sourceActions_;
  }

 private:
  Coordinator& coordinator_;
  playback_media_processing::Actions sourceActions_;
};

}  // namespace media_processing
