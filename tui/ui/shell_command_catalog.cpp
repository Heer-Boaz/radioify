#include "shell_command_catalog.h"

#include <string>

#include "browser_keymap.h"
#include "playback/input/shortcuts.h"

namespace shell_command_catalog {
namespace {

std::string playbackShortcutLabel(PlaybackAction action) {
  return std::string(playbackActionDisplayLabel(action));
}

std::string browserShortcutLabel(browser_input::KeyAction action) {
  return std::string(browser_input::keyActionDisplayLabel(action));
}

}  // namespace

Catalog build(const Context& context) {
  Catalog catalog;
  catalog.add("Play/Pause",
              playbackShortcutLabel(PlaybackAction::TogglePause),
              PlaybackAction::TogglePause);

  if (context.videoActive) {
    catalog.add("Toggle Playback Window",
                playbackShortcutLabel(PlaybackAction::ToggleWindow),
                PlaybackAction::ToggleWindow);
    catalog.add("Fullscreen",
                playbackShortcutLabel(PlaybackAction::ToggleFullscreen),
                PlaybackAction::ToggleFullscreen);
  }

  if (context.videoActive || context.pictureInPictureOpen ||
      context.audioAvailable) {
    catalog.add(
        "Picture-in-Picture",
        playbackShortcutLabel(PlaybackAction::TogglePictureInPicture),
        PlaybackAction::TogglePictureInPicture);
  }

  catalog.add("Cycle Radio Filter",
              playbackShortcutLabel(PlaybackAction::ToggleRadio),
              PlaybackAction::ToggleRadio);

  if (context.supports50Hz) {
    catalog.add("50 Hz Playback",
                playbackShortcutLabel(PlaybackAction::Toggle50Hz),
                PlaybackAction::Toggle50Hz);
  }

  if (context.pitchMonitorActive || context.audioAvailable) {
    catalog.add(context.pitchMonitorActive ? "Hide Pitch Monitor"
                                           : "Show Pitch Monitor",
                browserShortcutLabel(
                    browser_input::KeyAction::TogglePitchMonitor),
                PlaybackAction::TogglePitchMonitor);
  }

  if (!context.pitchMonitorActive) {
    const std::string viewShortcut =
        browserShortcutLabel(browser_input::KeyAction::CycleView);
    catalog.add("View: Grid", viewShortcut,
                SetBrowserView{BrowserState::ViewMode::Thumbnails});
    catalog.add("View: List", viewShortcut,
                SetBrowserView{BrowserState::ViewMode::ListOnly});
    catalog.add("View: Preview", viewShortcut,
                SetBrowserView{BrowserState::ViewMode::ListPreview});

    if (context.optionsAvailable) {
      catalog.add("Options",
                  playbackShortcutLabel(PlaybackAction::ToggleOptions),
                  PlaybackAction::ToggleOptions);
    }
    if (context.currentTargetAvailable) {
      catalog.add("Show Playing File", "", RevealPlayingFile{});
    }
  }

  catalog.add("Quit", playbackShortcutLabel(PlaybackAction::Quit),
              PlaybackAction::Quit);
  return catalog;
}

}  // namespace shell_command_catalog
