#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playback_media_actions {

// Commands that act on a media source rather than on a particular UI surface.
// Browser, playback, and editor menus project this same catalog into their own
// context instead of defining duplicate labels and availability rules.
enum class Action : uint8_t {
  Play,
  BrowseTracks,
  EditVideo,
  GenerateSubtitles,
  CancelSubtitleGeneration,
  AnalyzeAudio,
  SplitLoop,
};

enum class MediaKind : uint8_t {
  Unsupported,
  Audio,
  Video,
};

struct Context {
  MediaKind mediaKind = MediaKind::Unsupported;
  bool currentPlayback = false;
  bool editorActive = false;
  bool hasEdits = false;
  bool canBrowseTracks = false;
  bool canAnalyzeAudio = false;
  bool backgroundTaskRunning = false;
  bool hasGeneratedSubtitles = false;
  bool subtitleGenerationRunningForSource = false;
};

struct Item {
  Action action = Action::Play;
  std::string label;
};

std::vector<Item> build(const Context& context);

}  // namespace playback_media_actions
