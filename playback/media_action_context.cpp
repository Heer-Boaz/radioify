#include "playback/media_action_context.h"

#include "audio/media_formats.h"

namespace playback_media_actions {

MediaKind mediaKindForSource(const std::filesystem::path& sourceFile) {
  if (isSupportedVideoExt(sourceFile)) return MediaKind::Video;
  if (isSupportedAudioExt(sourceFile)) return MediaKind::Audio;
  return MediaKind::Unsupported;
}

}  // namespace playback_media_actions
