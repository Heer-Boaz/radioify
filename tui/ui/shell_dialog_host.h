#pragma once

#include <optional>
#include <variant>
#include <vector>

#include "application_exit.h"
#include "media_task_panel.h"
#include "playback_dialogs.h"
#include "shell_overlay_stack.h"

namespace tui_shell_dialogs {

// Domain outcomes produced by resolving a shell dialog. The host owns dialog
// identity and lifecycle only; the composition root remains responsible for
// executing these application actions.
using Event = std::variant<
    tui_application_exit::QuitNow, tui_application_exit::CancelTask,
    tui_media_task_panel::CancelTask, tui_media_task_panel::RetryTask,
    tui_media_task_panel::SetUpAudioSeparation,
    tui_playback_dialogs::AudioFallbackResolution>;

struct Interaction {
  bool consumed = false;
  bool changed = false;
  std::optional<tui_browser_media_menu::Command> mediaCommand;
  std::optional<shell_command_catalog::Intent> paletteIntent;
};

// Owns every input-modal dialog in the browser shell. Owner tokens and dialog
// leases never escape this boundary: opening, replacement, dismissal and
// result routing form one correlated workflow, while domain work is published
// as typed events for the application composition root.
class Host {
 public:
  explicit Host(shell_overlay_stack::Model& overlays);
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  Host(Host&&) = delete;
  Host& operator=(Host&&) = delete;

  void showInformation(tui_dialog::Content content);
  void showMediaTask(tui_media_task_panel::DialogRequest request);
  void showAudioFallback(
      const application_playback::AudioFallbackRequest& request);

  bool requestApplicationExit(
      const std::optional<playback_media_processing::Activity>& activeTask);
  bool synchronizeApplicationExit(
      const std::optional<playback_media_processing::Activity>& activeTask);
  bool resolveApplicationExitCancellation(
      playback_media_processing::TaskId taskId, bool accepted,
      const std::optional<playback_media_processing::Activity>& activeTask);

  bool synchronizeMediaTask(
      const std::optional<MediaTaskCardModel>& activeTask,
      const std::optional<MediaTaskFailureDialogModel>& latestFailure);
  bool revokeAudioFallback(
      application_playback::AudioFallbackDecisionId decision);
  bool dismissOverlays();

  Interaction handle(
      const InputEvent& event, const shell_overlay_stack::Bounds& bounds,
      const shell_command_catalog::Catalog& catalog,
      const std::optional<playback_media_processing::Activity>& activeTask);

  std::vector<Event> drainEvents();

 private:
  tui_dialog::DialogId open(shell_overlay_stack::DialogOwner owner,
                            tui_dialog::Content content);
  bool dismissDialog(tui_dialog::DialogId dialog);
  void retire(const shell_overlay_stack::DialogLease& lease);
  void route(
      const shell_overlay_stack::DialogResolution& resolution,
      const std::optional<playback_media_processing::Activity>& activeTask);
  bool apply(tui_application_exit::Transition transition);
  void publish(tui_application_exit::Intent intent);
  void publish(tui_media_task_panel::DialogIntent intent);

  shell_overlay_stack::Model& overlays_;
  shell_overlay_stack::DialogOwner informationOwner_;
  shell_overlay_stack::DialogOwner applicationExitOwner_;
  shell_overlay_stack::DialogOwner mediaTaskOwner_;
  shell_overlay_stack::DialogOwner audioFallbackOwner_;
  tui_application_exit::Controller applicationExit_;
  tui_media_task_panel::DialogSession mediaTaskDialogs_;
  tui_playback_dialogs::AudioFallbackSession audioFallbackDialog_;
  std::vector<Event> events_;
};

}  // namespace tui_shell_dialogs
