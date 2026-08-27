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
  VideoEditLeaveConfirmation = 1u << 8,
  VideoEditDiscardConfirmation = 1u << 9,
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
inline constexpr uint32_t kPlaybackShortcutContextVideoEditLeaveConfirmation =
    static_cast<uint32_t>(
        PlaybackShortcutContext::VideoEditLeaveConfirmation);
inline constexpr uint32_t
    kPlaybackShortcutContextVideoEditDiscardConfirmation =
        static_cast<uint32_t>(
            PlaybackShortcutContext::VideoEditDiscardConfirmation);
inline constexpr uint32_t kPlaybackShortcutContextAll =
    kPlaybackShortcutContextGlobal | kPlaybackShortcutContextShared |
    kPlaybackShortcutContextPlaybackSession |
    kPlaybackShortcutContextPictureInPicture |
    kPlaybackShortcutContextImageViewer |
     kPlaybackShortcutContextVideoPlayback |
     kPlaybackShortcutContextVideoEditing |
     kPlaybackShortcutContextVideoEditExitConfirmation |
     kPlaybackShortcutContextVideoEditLeaveConfirmation |
     kPlaybackShortcutContextVideoEditDiscardConfirmation;

enum class PlaybackAction : uint8_t {
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
  TogglePitchMonitor,
  SeekBackward,
  SeekForward,
  PreviousFrame,
  NextFrame,
  CopyVideoFrame,
  OpenVideoEditor,
  RequestCloseVideoEditor,
  NavigateBackInVideoEditor,
  ConfirmVideoEditPrompt,
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
