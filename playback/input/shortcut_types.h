#pragma once

#include <cstdint>

enum class PlaybackShortcutContext : uint32_t {
  Global = 1u << 0,
  Shared = 1u << 1,
  PlaybackSession = 1u << 2,
  PictureInPicture = 1u << 3,
  ImageViewer = 1u << 4,
  VideoPlayback = 1u << 5,
  VideoEditing = 1u << 6,
  VideoEditExitConfirmation = 1u << 7,
  VideoEditCloseConfirmation = 1u << 8,
};

inline constexpr uint32_t kPlaybackShortcutContextGlobal =
    static_cast<uint32_t>(PlaybackShortcutContext::Global);
inline constexpr uint32_t kPlaybackShortcutContextShared =
    static_cast<uint32_t>(PlaybackShortcutContext::Shared);
inline constexpr uint32_t kPlaybackShortcutContextPlaybackSession =
    static_cast<uint32_t>(PlaybackShortcutContext::PlaybackSession);
inline constexpr uint32_t kPlaybackShortcutContextPictureInPicture =
    static_cast<uint32_t>(PlaybackShortcutContext::PictureInPicture);
inline constexpr uint32_t kPlaybackShortcutContextImageViewer =
    static_cast<uint32_t>(PlaybackShortcutContext::ImageViewer);
inline constexpr uint32_t kPlaybackShortcutContextVideoPlayback =
    static_cast<uint32_t>(PlaybackShortcutContext::VideoPlayback);
inline constexpr uint32_t kPlaybackShortcutContextVideoEditing =
    static_cast<uint32_t>(PlaybackShortcutContext::VideoEditing);
inline constexpr uint32_t kPlaybackShortcutContextVideoEditExitConfirmation =
    static_cast<uint32_t>(
        PlaybackShortcutContext::VideoEditExitConfirmation);
inline constexpr uint32_t kPlaybackShortcutContextVideoEditCloseConfirmation =
    static_cast<uint32_t>(
        PlaybackShortcutContext::VideoEditCloseConfirmation);
inline constexpr uint32_t kPlaybackShortcutContextAll =
    kPlaybackShortcutContextGlobal | kPlaybackShortcutContextShared |
    kPlaybackShortcutContextPlaybackSession |
    kPlaybackShortcutContextPictureInPicture |
    kPlaybackShortcutContextImageViewer |
     kPlaybackShortcutContextVideoPlayback |
     kPlaybackShortcutContextVideoEditing |
     kPlaybackShortcutContextVideoEditExitConfirmation |
     kPlaybackShortcutContextVideoEditCloseConfirmation;

enum class PlaybackShortcutAction : uint8_t {
  Quit,
  Play,
  Pause,
  TogglePause,
  Stop,
  Previous,
  Next,
  ToggleWindow,
  ToggleFullscreen,
  ToggleRadio,
  Toggle50Hz,
  ToggleSubtitles,
  ToggleAudioTrack,
  ToggleOptions,
  SeekBackward,
  SeekForward,
  PreviousFrame,
  NextFrame,
  CopyVideoFrame,
  OpenVideoEditor,
  RequestCloseVideoEditor,
  NavigateBackInVideoEditor,
  ConfirmVideoEditorClose,
  SetVideoEditIn,
  SetVideoEditOut,
  ClearVideoEditIn,
  ClearVideoEditOut,
  ClearVideoEditInAndOut,
  RippleDeleteVideoEditSelection,
  TrimVideoEditSelection,
  UndoVideoEdit,
  RedoVideoEdit,
  ResetVideoEdits,
  ExportVideoEdits,
  DiscardVideoEditsAndExit,
  CancelVideoEditPrompt,
  VolumeUp,
  VolumeDown,
  TogglePictureInPicture,
  ExitPlaybackSession,
  DismissPictureInPicture,
  CloseViewer,
};
