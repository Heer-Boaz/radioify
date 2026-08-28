#include "tui/ui/shell_command_catalog.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "shell_command_catalog_tests: " << message << '\n';
  return false;
}

const tui_command_palette::Command* findCommand(
    const shell_command_catalog::Catalog& catalog, std::string_view label) {
  for (const tui_command_palette::Command& command : catalog.commands()) {
    if (command.label() == label) {
      return &command;
    }
  }
  return nullptr;
}

const shell_command_catalog::Intent* findIntent(
    const shell_command_catalog::Catalog& catalog, std::string_view label) {
  for (std::size_t index = 0; index < catalog.commands().size(); ++index) {
    if (catalog.commands()[index].label() == label) {
      return catalog.intentAt(index);
    }
  }
  return nullptr;
}

bool hasCommand(const shell_command_catalog::Catalog& catalog,
                std::string_view label) {
  return findCommand(catalog, label) != nullptr;
}

}  // namespace

int main() {
  using shell_command_catalog::Catalog;
  using shell_command_catalog::Context;
  bool ok = true;

  const Catalog idle = shell_command_catalog::build(Context{});
  ok &= expect(idle.commands().size() == 6,
               "idle browser must expose only universal and view commands");
  const auto* pause = findCommand(idle, "Play/Pause");
  const auto* grid = findCommand(idle, "View: Grid");
  const auto* quit = findCommand(idle, "Quit");
  ok &= expect(pause && pause->hotkey() == "Space" &&
                   grid && grid->hotkey() == "T" &&
                   quit && quit->hotkey() == "Ctrl+Q",
               "palette hints must come from the active shortcut catalogs");
  ok &= expect(!hasCommand(idle, "Picture-in-Picture") &&
                   !hasCommand(idle, "Show Pitch Monitor") &&
                   !hasCommand(idle, "Options"),
               "idle catalog must not advertise unavailable actions");

  Context active;
  active.videoActive = true;
  active.audioAvailable = true;
  active.supports50Hz = true;
  active.optionsAvailable = true;
  active.currentTargetAvailable = true;
  const Catalog media = shell_command_catalog::build(active);
  ok &= expect(hasCommand(media, "Toggle Playback Window") &&
                   hasCommand(media, "Fullscreen") &&
                   hasCommand(media, "Picture-in-Picture") &&
                   hasCommand(media, "50 Hz Playback") &&
                   hasCommand(media, "Show Pitch Monitor") &&
                   hasCommand(media, "Options") &&
                   hasCommand(media, "Show Playing File"),
               "active media context must expose every applicable command");

  const auto* viewIntent = findIntent(media, "View: Preview");
  const auto* optionsIntent = findIntent(media, "Options");
  ok &= expect(
      viewIntent &&
          std::get_if<shell_command_catalog::SetBrowserView>(viewIntent) &&
          std::get<shell_command_catalog::SetBrowserView>(*viewIntent).mode ==
              BrowserState::ViewMode::ListPreview &&
          optionsIntent && std::get_if<PlaybackAction>(optionsIntent) &&
          std::get<PlaybackAction>(*optionsIntent) ==
              PlaybackAction::ToggleOptions,
      "command and typed intent indices must remain aligned");
  ok &= expect(media.intentAt(media.commands().size()) == nullptr,
               "out-of-range palette selections must be rejected");

  Context tasks;
  tasks.activeMediaTaskCancellable = true;
  tasks.mediaTaskFailureAvailable = true;
  const Catalog taskCommands = shell_command_catalog::build(tasks);
  const auto* cancelTask =
      findIntent(taskCommands, "Cancel Background Task");
  const auto* showFailure =
      findIntent(taskCommands, "Show Last Task Error");
  ok &= expect(
      cancelTask &&
          std::get_if<shell_command_catalog::CancelMediaTask>(cancelTask) &&
          showFailure &&
          std::get_if<shell_command_catalog::ShowMediaTaskFailure>(
              showFailure),
      "background-task commands must use typed palette intents");

  active.pitchMonitorActive = true;
  const Catalog pitch = shell_command_catalog::build(active);
  ok &= expect(hasCommand(pitch, "Hide Pitch Monitor") &&
                   !hasCommand(pitch, "View: Grid") &&
                   !hasCommand(pitch, "Options") &&
                   !hasCommand(pitch, "Show Playing File"),
               "pitch-monitor mode must own the browser presentation layer");

  Context pictureInPictureOnly;
  pictureInPictureOnly.pictureInPictureOpen = true;
  const Catalog pip = shell_command_catalog::build(pictureInPictureOnly);
  ok &= expect(hasCommand(pip, "Picture-in-Picture"),
               "an open picture-in-picture window must remain controllable");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
