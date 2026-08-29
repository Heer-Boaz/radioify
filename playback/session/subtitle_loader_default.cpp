#include "subtitle_loader.h"

#include <utility>

namespace playback_session {

std::unique_ptr<SubtitleLoadService> createDefaultSubtitleLoadService() {
  return std::make_unique<SubtitleLoadService>(
      [](std::filesystem::path file,
         const SubtitleManager::CancellationCheck& cancelled)
          -> std::optional<SubtitleManager> {
        if (cancelled && cancelled()) return std::nullopt;
        SubtitleManager subtitles;
        subtitles.loadForVideo(file, cancelled);
        if (cancelled && cancelled()) return std::nullopt;
        return std::optional<SubtitleManager>(std::move(subtitles));
      });
}

}  // namespace playback_session
