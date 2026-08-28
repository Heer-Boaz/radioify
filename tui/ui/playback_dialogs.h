#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "tui/media_activation_decision.h"
#include "tui/ui/dialog.h"

namespace tui_playback_dialogs {

inline constexpr tui_dialog::ButtonId kPlayAudioButton = 1;
inline constexpr tui_dialog::ButtonId kCancelButton = 2;

tui_dialog::Content audioPlaybackFailure(const std::filesystem::path &file,
                                         std::string warning);
tui_dialog::Content pictureInPictureFailure(std::string detail);
tui_dialog::Content
videoPlaybackFailure(const std::filesystem::path &file,
                     const playback_session::Problem &problem);
tui_dialog::Content
audioFallback(const tui_media_activation::AudioFallbackRequest &request);

struct AudioFallbackResolution {
  tui_media_activation::DecisionId decision;
  bool playAudio = false;
};

// Correlates one input-modal prompt with the exact media-coordinator
// decision. Closing or replacing the prompt is an explicit decline, so the
// coordinator never retains a hidden queue activation.
class AudioFallbackSession {
public:
  void opened(tui_dialog::DialogId dialog,
              tui_media_activation::DecisionId decision);
  std::optional<AudioFallbackResolution>
  handle(const tui_dialog::ButtonActivation &activation);
  std::optional<AudioFallbackResolution> dismissed(tui_dialog::DialogId dialog);

private:
  std::optional<tui_dialog::DialogId> dialog_;
  std::optional<tui_media_activation::DecisionId> decision_;
};

} // namespace tui_playback_dialogs
