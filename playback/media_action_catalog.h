#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/audio_separation_availability.h"

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
  ExportTranscriptText,
  ExportAudio,
  CancelMediaExport,
  SetUpAudioSeparation,
  CancelAudioSeparationSetup,
  SeparateAudio,
  CancelAudioSeparation,
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
  playback_media_processing::AudioSeparationAvailability
      audioSeparationAvailability =
          playback_media_processing::AudioSeparationAvailability::Unavailable;
  bool canExportAudio = false;
  bool canExportTranscriptText = false;
  bool canGenerateSubtitles = false;
  bool backgroundTaskRunning = false;
  bool activeTaskCancellable = false;
  bool hasGeneratedSubtitles = false;
  bool subtitleGenerationRunningForSource = false;
  bool hasSeparatedAudio = false;
  bool audioSeparationSetupRunningForSource = false;
  bool audioSeparationRunningForSource = false;
  bool audioExportRunningForSource = false;
  bool transcriptTextExportRunningForSource = false;
};

struct Item {
  Action action = Action::Play;
  std::string label;
};

std::vector<Item> build(const Context& context);

}  // namespace playback_media_actions
