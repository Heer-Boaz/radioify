#include "videoplayback.h"

#include <filesystem>

#include "playback_session.h"

bool showAsciiVideo(const std::filesystem::path& file, ConsoleInput& input,
                    ConsoleScreen& screen, const Style& baseStyle,
                    const Style& accentStyle, const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart, const Color& progressEnd,
                    const VideoPlaybackConfig& config,
                    bool* quitAppRequested,
                    PlaybackSystemControls* systemControls,
                    std::function<bool(PlaybackTransportCommand)>
                        requestTransportCommand,
                    PlaybackSessionContinuationState* continuityState) {
  PlaybackSession session({file,
                           input,
                           screen,
                           baseStyle,
                           accentStyle,
                           dimStyle,
                           progressEmptyStyle,
                           progressFrameStyle,
                           progressStart,
                           progressEnd,
                           config,
                           quitAppRequested,
                           systemControls,
                           std::move(requestTransportCommand),
                           continuityState});
  return session.run();
}
