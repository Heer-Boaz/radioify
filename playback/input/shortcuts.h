#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "consoleinput.h"
#include "playback/input/media_keys.h"
#include "playback/input/shortcut_types.h"
#include "playback/video/edit/command.h"
#include "shortcut_match.h"

struct PlaybackShortcutBinding {
  PlaybackShortcutAction action = PlaybackShortcutAction::TogglePause;
  WORD vk = 0;
  char lower = 0;
  char upper = 0;
  DWORD requiredModifierMask = 0;
  DWORD forbiddenModifierMask = 0;
  uint32_t contexts = kPlaybackShortcutContextAll;
};

inline constexpr DWORD kPlaybackShortcutCtrlMask = kShortcutCtrlMask;
inline constexpr DWORD kPlaybackShortcutAltMask = kShortcutAltMask;
inline constexpr DWORD kPlaybackShortcutShiftMask = kShortcutShiftMask;

inline constexpr std::optional<playback_video_edit::Command>
videoEditCommandForShortcut(PlaybackShortcutAction action) {
  using Command = playback_video_edit::Command;
  switch (action) {
    case PlaybackShortcutAction::OpenVideoEditor:
      return Command::Open;
    case PlaybackShortcutAction::RequestCloseVideoEditor:
      return Command::RequestClose;
    case PlaybackShortcutAction::ConfirmVideoEditPrompt:
      return Command::ConfirmPrompt;
    case PlaybackShortcutAction::SetVideoEditIn:
      return Command::MarkIn;
    case PlaybackShortcutAction::SetVideoEditOut:
      return Command::MarkOut;
    case PlaybackShortcutAction::ClearVideoEditIn:
      return Command::ClearIn;
    case PlaybackShortcutAction::ClearVideoEditOut:
      return Command::ClearOut;
    case PlaybackShortcutAction::ClearVideoEditInAndOut:
      return Command::ClearInAndOut;
    case PlaybackShortcutAction::RippleDeleteVideoEditSelection:
      return Command::RippleDelete;
    case PlaybackShortcutAction::TrimVideoEditSelection:
      return Command::Trim;
    case PlaybackShortcutAction::UndoVideoEdit:
      return Command::Undo;
    case PlaybackShortcutAction::RedoVideoEdit:
      return Command::Redo;
    case PlaybackShortcutAction::ResetVideoEdits:
      return Command::Reset;
    case PlaybackShortcutAction::ExportVideoEdits:
      return Command::StartExport;
    default:
      return std::nullopt;
  }
}
inline constexpr DWORD kPlaybackShortcutTextForbiddenMask =
    kShortcutTextForbiddenMask;
inline constexpr DWORD kPlaybackShortcutChordForbiddenMask =
    kShortcutChordForbiddenMask;
inline constexpr DWORD kPlaybackShortcutSeekForbiddenMask =
    kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask;
inline constexpr DWORD kPlaybackShortcutFrameStepForbiddenMask =
    kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask;

// One shared shortcut table. Context masks let modes layer additional keys on
// top of the shared map without owning separate per-mode tables.
inline constexpr std::array<PlaybackShortcutBinding, 55>
    kPlaybackShortcutBindings = {{
        {PlaybackShortcutAction::Quit, 'Q', 'q', 'Q', kPlaybackShortcutCtrlMask,
         kPlaybackShortcutChordForbiddenMask, kPlaybackShortcutContextGlobal},
        {PlaybackShortcutAction::TogglePictureInPicture, 'P', 'p', 'P',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared |
             kPlaybackShortcutContextPlaybackSession |
              kPlaybackShortcutContextPictureInPicture},
        {PlaybackShortcutAction::CancelVideoEditPrompt, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditLeaveConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation},
        {PlaybackShortcutAction::CancelVideoEditPrompt, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditLeaveConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation},
        {PlaybackShortcutAction::ConfirmVideoEditPrompt, VK_RETURN, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditLeaveConfirmation},
        {PlaybackShortcutAction::CancelVideoEditPrompt, VK_RETURN, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation},
        {PlaybackShortcutAction::ConfirmVideoEditPrompt, 'D', 'd', 'D', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditDiscardConfirmation},
        {PlaybackShortcutAction::DiscardVideoEditsAndExit, 'D', 'd', 'D', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditExitConfirmation},
        // Editing is an explicit modal layer. Conflicting bindings precede
        // shared playback so O and Back regain their normal meanings as soon
        // as the editor is closed.
        {PlaybackShortcutAction::NavigateBackInVideoEditor, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::NavigateBackInVideoEditor, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::RequestCloseVideoEditor, 'E', 'e', 'E', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ExportVideoEdits, 'E', 'e', 'E',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing |
              kPlaybackShortcutContextVideoPlayback |
              kPlaybackShortcutContextVideoEditExitConfirmation},
        {PlaybackShortcutAction::UndoVideoEdit, 'Z', 'z', 'Z',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::RedoVideoEdit, 'Y', 'y', 'Y',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ResetVideoEdits, 'R', 'r', 'R',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::SetVideoEditIn, 'I', 'i', 'I', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::SetVideoEditOut, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ClearVideoEditIn, 'I', 'i', 'I',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ClearVideoEditOut, 'O', 'o', 'O',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ClearVideoEditInAndOut, 'X', 'x', 'X',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::RippleDeleteVideoEditSelection, VK_DELETE, 0,
         0, 0, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::TrimVideoEditSelection, 'T', 't', 'T', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::DismissPictureInPicture, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackShortcutAction::DismissPictureInPicture, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackShortcutAction::DismissPictureInPicture, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackShortcutAction::CloseViewer, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackShortcutAction::CloseViewer, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackShortcutAction::Play, kPlaybackVkMediaPlay, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Pause, kPlaybackVkMediaPause, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::TogglePause, VK_SPACE, ' ', ' ',
         0, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::TogglePause, VK_MEDIA_PLAY_PAUSE, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Stop, VK_MEDIA_STOP, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Previous, VK_MEDIA_PREV_TRACK, 0, 0, 0,
         0, kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Next, VK_MEDIA_NEXT_TRACK, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        // Shared navigation layer:
        //   - Left/Right arrows seek within the current item.
        //   - Ctrl+Left/Right move to the previous/next item in the playlist.
        {PlaybackShortcutAction::Previous, VK_LEFT, 0, 0,
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Next, VK_RIGHT, 0, 0, kPlaybackShortcutCtrlMask,
         kPlaybackShortcutChordForbiddenMask, kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleWindow, 'W', 'w', 'W',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleFullscreen, VK_RETURN, 0, 0,
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ToggleRadio, 'R', 'r', 'R', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Toggle50Hz, 'H', 'h', 'H', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleSubtitles, 'S', 's', 'S', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleAudioTrack, 'A', 'a', 'A', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleOptions, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekBackward, VK_OEM_4, '[', '[', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekForward, VK_OEM_6, ']', ']', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekBackward, VK_LEFT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekForward, VK_RIGHT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::PreviousFrame, VK_OEM_COMMA, ',', ',', 0,
         kPlaybackShortcutFrameStepForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackShortcutAction::NextFrame, VK_OEM_PERIOD, '.', '.', 0,
         kPlaybackShortcutFrameStepForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackShortcutAction::CopyVideoFrame, 'S', 's', 'S',
         kPlaybackShortcutShiftMask, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackShortcutAction::OpenVideoEditor, 'E', 'e', 'E', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackShortcutAction::VolumeUp, VK_UP, 0, 0, kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::VolumeDown, VK_DOWN, 0, 0,
         kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared},
    }};

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    const KeyEvent& key,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  for (const PlaybackShortcutBinding& binding : kPlaybackShortcutBindings) {
    if ((binding.contexts & shortcutContexts) == 0) {
      continue;
    }
    if (matchesShortcut(key, binding.vk, binding.lower, binding.upper,
                        binding.requiredModifierMask,
                        binding.forbiddenModifierMask)) {
      return binding.action;
    }
  }
  return std::nullopt;
}

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    InputAction action, uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                                    kPlaybackShortcutContextShared) {
  switch (action) {
    case InputAction::Back:
      if ((shortcutContexts &
           kPlaybackShortcutContextVideoEditLeaveConfirmation) != 0 ||
          (shortcutContexts &
           kPlaybackShortcutContextVideoEditDiscardConfirmation) != 0) {
        return PlaybackShortcutAction::CancelVideoEditPrompt;
      }
      if ((shortcutContexts &
           kPlaybackShortcutContextVideoEditExitConfirmation) != 0) {
        return PlaybackShortcutAction::CancelVideoEditPrompt;
      }
      if ((shortcutContexts & kPlaybackShortcutContextVideoEditing) != 0) {
        return PlaybackShortcutAction::NavigateBackInVideoEditor;
      }
      if ((shortcutContexts & kPlaybackShortcutContextPlaybackSession) != 0) {
        return PlaybackShortcutAction::ExitPlaybackSession;
      }
      if ((shortcutContexts & kPlaybackShortcutContextPictureInPicture) != 0) {
        return PlaybackShortcutAction::DismissPictureInPicture;
      }
      if ((shortcutContexts & kPlaybackShortcutContextImageViewer) != 0) {
        return PlaybackShortcutAction::CloseViewer;
      }
      return std::nullopt;
    case InputAction::Forward:
      return std::nullopt;
  }
  return std::nullopt;
}

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    const InputEvent& ev,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  if (ev.type == InputEvent::Type::Action) {
    return resolvePlaybackShortcutAction(ev.action, shortcutContexts);
  }
  if (ev.type != InputEvent::Type::Key) {
    return std::nullopt;
  }
  return resolvePlaybackShortcutAction(ev.key, shortcutContexts);
}
