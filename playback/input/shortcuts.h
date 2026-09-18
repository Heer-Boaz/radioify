#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "input_event.h"
#include "playback/input/command.h"
#include "playback/input/media_keys.h"
#include "playback/input/shortcut_types.h"
#include "playback/video/edit/command.h"
#include "shortcut_match.h"

enum class PlaybackShortcutScope : std::uint8_t {
  // Subject to the focused UI mode selected by the caller's context mask.
  Contextual,
  // Process transport emitted by hardware or the operating system; callers
  // may route this scope above focused text without promoting keyboard
  // shortcuts such as Space or Ctrl+Left.
  SystemMedia,
};

struct PlaybackShortcutBinding {
  constexpr PlaybackShortcutBinding() = default;

  constexpr PlaybackShortcutBinding(
      PlaybackAction actionValue, WORD virtualKey, char lowerCharacter,
      char upperCharacter, DWORD requiredModifiers,
      DWORD forbiddenModifiers, uint32_t shortcutContexts,
      std::string_view label = {},
      ShortcutRepeatPolicy repeat =
          ShortcutRepeatPolicy::InitialPressOnly,
      PlaybackShortcutScope shortcutScope =
          PlaybackShortcutScope::Contextual)
      : action(actionValue),
        vk(virtualKey),
        lower(lowerCharacter),
        upper(upperCharacter),
        requiredModifierMask(requiredModifiers),
        forbiddenModifierMask(forbiddenModifiers),
        contexts(shortcutContexts),
        displayLabel(label),
        repeatPolicy(repeat),
        scope(shortcutScope) {}

  PlaybackAction action = PlaybackAction::TogglePause;
  WORD vk = 0;
  char lower = 0;
  char upper = 0;
  DWORD requiredModifierMask = 0;
  DWORD forbiddenModifierMask = 0;
  uint32_t contexts = kPlaybackShortcutContextAll;
  std::string_view displayLabel;
  ShortcutRepeatPolicy repeatPolicy =
      ShortcutRepeatPolicy::InitialPressOnly;
  PlaybackShortcutScope scope = PlaybackShortcutScope::Contextual;
};

inline constexpr DWORD kPlaybackShortcutCtrlMask = kShortcutCtrlMask;
inline constexpr DWORD kPlaybackShortcutAltMask = kShortcutAltMask;
inline constexpr DWORD kPlaybackShortcutShiftMask = kShortcutShiftMask;

inline constexpr std::optional<playback_video_edit::Command>
videoEditCommandForShortcut(PlaybackAction action) {
  using Command = playback_video_edit::Command;
  switch (action) {
    case PlaybackAction::OpenVideoEditor:
      return Command::Open;
    case PlaybackAction::RequestCloseVideoEditor:
      return Command::RequestClose;
    case PlaybackAction::ConfirmVideoEditPrompt:
      return Command::ConfirmPrompt;
    case PlaybackAction::SetVideoEditIn:
      return Command::MarkIn;
    case PlaybackAction::SetVideoEditOut:
      return Command::MarkOut;
    case PlaybackAction::ClearVideoEditIn:
      return Command::ClearIn;
    case PlaybackAction::ClearVideoEditOut:
      return Command::ClearOut;
    case PlaybackAction::ClearVideoEditInAndOut:
      return Command::ClearInAndOut;
    case PlaybackAction::RippleDeleteVideoEditSelection:
      return Command::RippleDelete;
    case PlaybackAction::TrimVideoEditSelection:
      return Command::Trim;
    case PlaybackAction::UndoVideoEdit:
      return Command::Undo;
    case PlaybackAction::RedoVideoEdit:
      return Command::Redo;
    case PlaybackAction::ResetVideoEdits:
      return Command::Reset;
    case PlaybackAction::ExportVideoEdits:
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
inline constexpr std::array<PlaybackShortcutBinding, 64>
    kPlaybackShortcutBindings = {{
        {PlaybackAction::Quit, 'Q', 'q', 'Q', kPlaybackShortcutCtrlMask,
         kPlaybackShortcutChordForbiddenMask, kPlaybackShortcutContextGlobal,
         "Ctrl+Q"},
        {PlaybackAction::TogglePictureInPicture, 'P', 'p', 'P',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared |
             kPlaybackShortcutContextPlaybackSession |
              kPlaybackShortcutContextPictureInPicture,
         "Ctrl+P"},
        {PlaybackAction::CancelVideoEditPrompt, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditLeaveConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation |
             kPlaybackShortcutContextVideoEditRestartConfirmation},
        {PlaybackAction::CancelVideoEditPrompt, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditLeaveConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation |
             kPlaybackShortcutContextVideoEditRestartConfirmation},
        {PlaybackAction::DismissMediaActionConfirmation, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::DismissMediaActionConfirmation, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::SelectPreviousMediaActionConfirmation, VK_LEFT,
         0, 0, 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::SelectNextMediaActionConfirmation, VK_RIGHT,
         0, 0, 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::SelectNextMediaActionConfirmation, VK_TAB,
         0, 0, 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::SelectPreviousMediaActionConfirmation, VK_TAB,
         0, 0, kPlaybackShortcutShiftMask,
         kPlaybackShortcutTextForbiddenMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::ActivateMediaActionConfirmation, VK_RETURN,
         0, 0, 0, kPlaybackShortcutTextForbiddenMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::ActivateMediaActionConfirmation, VK_SPACE,
         ' ', ' ', 0, kPlaybackShortcutTextForbiddenMask,
          kPlaybackShortcutContextMediaActionConfirmation},
        {PlaybackAction::ConfirmVideoEditPrompt, VK_RETURN, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditLeaveConfirmation},
        {PlaybackAction::CancelVideoEditPrompt, VK_RETURN, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditExitConfirmation |
             kPlaybackShortcutContextVideoEditDiscardConfirmation |
             kPlaybackShortcutContextVideoEditRestartConfirmation},
        {PlaybackAction::ConfirmVideoEditPrompt, 'R', 'r', 'R', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditRestartConfirmation},
        {PlaybackAction::ConfirmVideoEditPrompt, 'D', 'd', 'D', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditDiscardConfirmation},
        {PlaybackAction::DiscardVideoEditsAndExit, 'D', 'd', 'D', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditExitConfirmation},
        // Editing is an explicit modal layer. Conflicting bindings precede
        // shared playback so O and Back regain their normal meanings as soon
        // as the editor is closed.
        {PlaybackAction::NavigateBackInVideoEditor, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::NavigateBackInVideoEditor, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::RequestCloseVideoEditor, 'E', 'e', 'E', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ExportVideoEdits, 'E', 'e', 'E',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing |
              kPlaybackShortcutContextVideoPlayback |
              kPlaybackShortcutContextVideoEditExitConfirmation},
        {PlaybackAction::UndoVideoEdit, 'Z', 'z', 'Z',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::RedoVideoEdit, 'Y', 'y', 'Y',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ResetVideoEdits, 'R', 'r', 'R',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::SetVideoEditIn, 'I', 'i', 'I', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::SetVideoEditOut, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ClearVideoEditIn, 'I', 'i', 'I',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ClearVideoEditOut, 'O', 'o', 'O',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ClearVideoEditInAndOut, 'X', 'x', 'X',
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::RippleDeleteVideoEditSelection, VK_DELETE, 0,
         0, 0, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::TrimVideoEditSelection, 'T', 't', 'T', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoEditing},
        {PlaybackAction::ExitPlaybackSession, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackAction::ExitPlaybackSession, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackAction::DismissPictureInPicture, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackAction::DismissPictureInPicture, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackAction::DismissPictureInPicture, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPictureInPicture},
        {PlaybackAction::CloseViewer, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackAction::CloseViewer, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackAction::Play, kPlaybackVkMediaPlay, 0, 0, 0, 0,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::Pause, kPlaybackVkMediaPause, 0, 0, 0, 0,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::TogglePause, VK_SPACE, ' ', ' ',
         0, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared, "Space"},
        {PlaybackAction::TogglePause, VK_MEDIA_PLAY_PAUSE, 0, 0, 0, 0,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::Stop, VK_MEDIA_STOP, 0, 0, 0, 0,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::Previous, VK_MEDIA_PREV_TRACK, 0, 0, 0,
         0, kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::Next, VK_MEDIA_NEXT_TRACK, 0, 0, 0, 0,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::InitialPressOnly,
         PlaybackShortcutScope::SystemMedia},
        {PlaybackAction::ToggleWindow, 'W', 'w', 'W',
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared, "Ctrl+W"},
        {PlaybackAction::ToggleFullscreen, VK_RETURN, 0, 0,
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextPlaybackSession, "Alt+Enter"},
        {PlaybackAction::ToggleRadio, 'R', 'r', 'R', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared, "R"},
        {PlaybackAction::Toggle50Hz, 'H', 'h', 'H', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared, "H"},
        {PlaybackAction::ToggleSubtitles, 'S', 's', 'S', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextShared},
        {PlaybackAction::ToggleAudioTrack, 'A', 'a', 'A', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackAction::ToggleOptions, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared, "O"},
        {PlaybackAction::SeekBackward, VK_OEM_4, '[', '[', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
        {PlaybackAction::SeekForward, VK_OEM_6, ']', ']', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
        {PlaybackAction::SeekBackward, VK_LEFT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
        {PlaybackAction::SeekForward, VK_RIGHT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
        {PlaybackAction::PreviousFrame, VK_OEM_COMMA, ',', ',', 0,
         kPlaybackShortcutFrameStepForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackAction::NextFrame, VK_OEM_PERIOD, '.', '.', 0,
         kPlaybackShortcutFrameStepForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackAction::CopyVideoFrame, 'S', 's', 'S',
         kPlaybackShortcutShiftMask, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackAction::OpenVideoEditor, 'E', 'e', 'E', 0,
         kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextVideoPlayback},
        {PlaybackAction::VolumeUp, VK_UP, 0, 0, kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
        {PlaybackAction::VolumeDown, VK_DOWN, 0, 0,
         kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared, {},
         ShortcutRepeatPolicy::AllowAutoRepeat},
    }};

inline constexpr std::string_view playbackActionDisplayLabel(
    PlaybackAction action) {
  for (const PlaybackShortcutBinding& binding :
       kPlaybackShortcutBindings) {
    if (binding.action == action && !binding.displayLabel.empty()) {
      return binding.displayLabel;
    }
  }
  return {};
}

struct PlaybackActionMatch {
  PlaybackAction action = PlaybackAction::TogglePause;
  std::uint32_t repetitions = 1;
};

inline const PlaybackShortcutBinding* resolvePlaybackShortcutBinding(
    const KeyEvent& key,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared,
    std::optional<PlaybackShortcutScope> requiredScope = std::nullopt) {
  for (const PlaybackShortcutBinding& binding : kPlaybackShortcutBindings) {
    if ((binding.contexts & shortcutContexts) == 0) {
      continue;
    }
    if (requiredScope && binding.scope != *requiredScope) {
      continue;
    }
    if (matchesShortcut(key, binding.vk, binding.lower, binding.upper,
                        binding.requiredModifierMask,
                        binding.forbiddenModifierMask,
                        binding.repeatPolicy)) {
      return &binding;
    }
  }
  return nullptr;
}

inline std::optional<PlaybackAction> resolvePlaybackAction(
    const KeyEvent& key,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  const PlaybackShortcutBinding* binding =
      resolvePlaybackShortcutBinding(key, shortcutContexts);
  return binding ? std::optional<PlaybackAction>(binding->action)
                 : std::nullopt;
}

inline std::optional<PlaybackAction> resolvePlaybackAction(
    InputAction action, uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                                    kPlaybackShortcutContextShared) {
  switch (action) {
    case InputAction::Back:
      if ((shortcutContexts &
           kPlaybackShortcutContextMediaActionConfirmation) != 0) {
        return PlaybackAction::DismissMediaActionConfirmation;
      }
      if ((shortcutContexts &
           kPlaybackShortcutContextVideoEditLeaveConfirmation) != 0 ||
          (shortcutContexts &
           kPlaybackShortcutContextVideoEditDiscardConfirmation) != 0 ||
          (shortcutContexts &
           kPlaybackShortcutContextVideoEditRestartConfirmation) != 0) {
        return PlaybackAction::CancelVideoEditPrompt;
      }
      if ((shortcutContexts &
           kPlaybackShortcutContextVideoEditExitConfirmation) != 0) {
        return PlaybackAction::CancelVideoEditPrompt;
      }
      if ((shortcutContexts & kPlaybackShortcutContextVideoEditing) != 0) {
        return PlaybackAction::NavigateBackInVideoEditor;
      }
      if ((shortcutContexts & kPlaybackShortcutContextPlaybackSession) != 0) {
        return PlaybackAction::ExitPlaybackSession;
      }
      if ((shortcutContexts & kPlaybackShortcutContextPictureInPicture) != 0) {
        return PlaybackAction::DismissPictureInPicture;
      }
      if ((shortcutContexts & kPlaybackShortcutContextImageViewer) != 0) {
        return PlaybackAction::CloseViewer;
      }
      return std::nullopt;
    case InputAction::Forward:
      return std::nullopt;
  }
  return std::nullopt;
}

inline std::optional<PlaybackAction> resolvePlaybackAction(
    const InputEvent& ev,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  if (ev.type == InputEvent::Type::Action) {
    return resolvePlaybackAction(ev.action, shortcutContexts);
  }
  if (ev.type != InputEvent::Type::Key) {
    return std::nullopt;
  }
  return resolvePlaybackAction(ev.key, shortcutContexts);
}

inline std::optional<PlaybackActionMatch> resolvePlaybackActionMatch(
    const InputEvent& event,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared,
    std::optional<PlaybackShortcutScope> requiredScope = std::nullopt) {
  if (event.type == InputEvent::Type::Action) {
    if (requiredScope &&
        *requiredScope != PlaybackShortcutScope::Contextual) {
      return std::nullopt;
    }
    const std::optional<PlaybackAction> action =
        resolvePlaybackAction(event.action, shortcutContexts);
    return action ? std::optional<PlaybackActionMatch>(
                        PlaybackActionMatch{*action, 1})
                  : std::nullopt;
  }
  if (event.type != InputEvent::Type::Key) {
    return std::nullopt;
  }
  const PlaybackShortcutBinding* binding =
      resolvePlaybackShortcutBinding(event.key, shortcutContexts,
                                     requiredScope);
  if (!binding) return std::nullopt;
  const std::uint32_t repetitions =
      binding->repeatPolicy == ShortcutRepeatPolicy::AllowAutoRepeat
          ? keyPressCount(event.key)
          : 1;
  return PlaybackActionMatch{binding->action, repetitions};
}

namespace playback_input {

inline Command commandForShortcut(const PlaybackActionMatch& match) {
  const std::uint32_t repetitions = match.repetitions == 0
                                        ? 1
                                        : match.repetitions;
  const int steps = static_cast<int>(std::min<std::uint32_t>(
      repetitions,
      static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
  switch (match.action) {
    case PlaybackAction::SeekBackward:
      return SeekBySteps{-steps};
    case PlaybackAction::SeekForward:
      return SeekBySteps{steps};
    case PlaybackAction::VolumeUp:
      return AdjustVolume{
          0.10f * static_cast<float>(std::min(10, steps))};
    case PlaybackAction::VolumeDown:
      return AdjustVolume{
          -0.10f * static_cast<float>(std::min(10, steps))};
    default:
      return match.action;
  }
}

}  // namespace playback_input

// Playback keeps a deliberately small application-level shortcut layer while
// the media browser owns the terminal. Text-entry modes can suppress this
// layer; browser back/up remains browser navigation, while Escape still closes
// the active video session.
inline std::optional<PlaybackAction>
resolveLiveBrowserVideoShortcut(const InputEvent& event) {
  constexpr uint32_t kContexts =
      kPlaybackShortcutContextGlobal | kPlaybackShortcutContextShared |
      kPlaybackShortcutContextPlaybackSession |
      kPlaybackShortcutContextVideoPlayback;
  const std::optional<PlaybackAction> action =
      resolvePlaybackAction(event, kContexts);
  if (!action) return std::nullopt;

  switch (*action) {
    case PlaybackAction::Quit:
    case PlaybackAction::Play:
    case PlaybackAction::Pause:
    case PlaybackAction::TogglePause:
    case PlaybackAction::Stop:
    case PlaybackAction::Previous:
    case PlaybackAction::Next:
    case PlaybackAction::ToggleWindow:
    case PlaybackAction::ToggleFullscreen:
    case PlaybackAction::ToggleRadio:
    case PlaybackAction::Toggle50Hz:
    case PlaybackAction::ToggleSubtitles:
    case PlaybackAction::ToggleAudioTrack:
    case PlaybackAction::SeekBackward:
    case PlaybackAction::SeekForward:
    case PlaybackAction::PreviousFrame:
    case PlaybackAction::NextFrame:
    case PlaybackAction::CopyVideoFrame:
    case PlaybackAction::OpenVideoEditor:
    case PlaybackAction::VolumeUp:
    case PlaybackAction::VolumeDown:
    case PlaybackAction::TogglePictureInPicture:
      return action;
    case PlaybackAction::ExitPlaybackSession:
      return event.type == InputEvent::Type::Key && event.key.vk == VK_ESCAPE
                 ? action
                 : std::nullopt;
    case PlaybackAction::ToggleOptions:
    case PlaybackAction::TogglePitchMonitor:
    case PlaybackAction::RequestCloseVideoEditor:
    case PlaybackAction::NavigateBackInVideoEditor:
    case PlaybackAction::ConfirmVideoEditPrompt:
    case PlaybackAction::SetVideoEditIn:
    case PlaybackAction::SetVideoEditOut:
    case PlaybackAction::ClearVideoEditIn:
    case PlaybackAction::ClearVideoEditOut:
    case PlaybackAction::ClearVideoEditInAndOut:
    case PlaybackAction::RippleDeleteVideoEditSelection:
    case PlaybackAction::TrimVideoEditSelection:
    case PlaybackAction::UndoVideoEdit:
    case PlaybackAction::RedoVideoEdit:
    case PlaybackAction::ResetVideoEdits:
    case PlaybackAction::ExportVideoEdits:
    case PlaybackAction::DiscardVideoEditsAndExit:
    case PlaybackAction::CancelVideoEditPrompt:
    case PlaybackAction::SelectPreviousMediaActionConfirmation:
    case PlaybackAction::SelectNextMediaActionConfirmation:
    case PlaybackAction::ActivateMediaActionConfirmation:
    case PlaybackAction::DismissMediaActionConfirmation:
    case PlaybackAction::DismissPictureInPicture:
    case PlaybackAction::CloseViewer:
      return std::nullopt;
  }
  return std::nullopt;
}
