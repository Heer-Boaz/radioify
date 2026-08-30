#include "shell_dialog_host.h"

#include <cassert>
#include <utility>

namespace tui_shell_dialogs {

Host::Host(shell_overlay_stack::Model& overlays)
    : overlays_(overlays),
      informationOwner_(overlays_.createDialogOwner()),
      applicationExitOwner_(overlays_.createDialogOwner()),
      mediaTaskOwner_(overlays_.createDialogOwner()),
      audioFallbackOwner_(overlays_.createDialogOwner()) {}

void Host::showInformation(tui_dialog::Content content) {
  open(informationOwner_, std::move(content));
}

void Host::showMediaTask(tui_media_task_panel::DialogRequest request) {
  const tui_dialog::DialogId dialog =
      open(mediaTaskOwner_, std::move(request.content));
  mediaTaskDialogs_.opened(dialog, std::move(request.context));
}

void Host::showAudioFallback(
    const application_playback::AudioFallbackRequest& request) {
  const tui_dialog::DialogId dialog =
      open(audioFallbackOwner_, tui_playback_dialogs::audioFallback(request));
  audioFallbackDialog_.opened(dialog, request.id);
}

bool Host::requestApplicationExit(
    const std::optional<playback_media_processing::Activity>& activeTask) {
  return apply(applicationExit_.request(activeTask));
}

bool Host::synchronizeApplicationExit(
    const std::optional<playback_media_processing::Activity>& activeTask) {
  return apply(applicationExit_.synchronize(activeTask));
}

bool Host::resolveApplicationExitCancellation(
    playback_media_processing::TaskId taskId, bool accepted,
    const std::optional<playback_media_processing::Activity>& activeTask) {
  return apply(
      applicationExit_.resolveCancellation(taskId, accepted, activeTask));
}

bool Host::synchronizeMediaTask(
    const std::optional<MediaTaskCardModel>& activeTask,
    const std::optional<MediaTaskFailureDialogModel>& latestFailure) {
  const std::optional<tui_dialog::DialogId> obsolete =
      mediaTaskDialogs_.synchronize(activeTask, latestFailure);
  return obsolete && dismissDialog(*obsolete);
}

bool Host::revokeAudioFallback(
    application_playback::AudioFallbackDecisionId decision) {
  const std::optional<tui_dialog::DialogId> dialog =
      audioFallbackDialog_.revoke(decision);
  return dialog && dismissDialog(*dialog);
}

bool Host::dismissOverlays() {
  const shell_overlay_stack::Dismissal dismissal = overlays_.dismiss();
  if (dismissal.dialog) {
    retire(*dismissal.dialog);
  }
  return dismissal.changed;
}

Interaction Host::handle(
    const InputEvent& event, const shell_overlay_stack::Bounds& bounds,
    const shell_command_catalog::Catalog& catalog,
    const std::optional<playback_media_processing::Activity>& activeTask) {
  shell_overlay_stack::Interaction interaction =
      overlays_.handle(event, bounds, catalog);
  if (interaction.dialogResolution) {
    route(*interaction.dialogResolution, activeTask);
  }

  Interaction result;
  result.consumed = interaction.consumed;
  result.changed = interaction.changed;
  result.mediaCommand = std::move(interaction.mediaCommand);
  result.paletteIntent = std::move(interaction.paletteIntent);
  return result;
}

std::vector<Event> Host::drainEvents() {
  std::vector<Event> result;
  result.swap(events_);
  return result;
}

tui_dialog::DialogId Host::open(shell_overlay_stack::DialogOwner owner,
                                tui_dialog::Content content) {
  shell_overlay_stack::DialogOpening opening =
      overlays_.openDialog(owner, std::move(content));
  if (opening.replaced) {
    retire(*opening.replaced);
  }
  return opening.lease.dialog;
}

bool Host::dismissDialog(tui_dialog::DialogId dialog) {
  const std::optional<shell_overlay_stack::DialogLease> dismissed =
      overlays_.dismissDialog(dialog);
  if (!dismissed) {
    return false;
  }
  retire(*dismissed);
  return true;
}

void Host::retire(const shell_overlay_stack::DialogLease& lease) {
  if (lease.owner == applicationExitOwner_) {
    applicationExit_.dismissed(lease.dialog);
    return;
  }
  if (lease.owner == mediaTaskOwner_) {
    mediaTaskDialogs_.dismissed(lease.dialog);
    return;
  }
  if (lease.owner == audioFallbackOwner_) {
    if (std::optional<tui_playback_dialogs::AudioFallbackResolution>
            resolution = audioFallbackDialog_.dismissed(lease.dialog)) {
      events_.emplace_back(*resolution);
    }
    return;
  }
  assert(lease.owner == informationOwner_ &&
         "all shell dialogs must be opened through the dialog host");
}

void Host::route(
    const shell_overlay_stack::DialogResolution& resolution,
    const std::optional<playback_media_processing::Activity>& activeTask) {
  if (resolution.activatedButton) {
    const tui_dialog::ButtonActivation activation{resolution.lease.dialog,
                                                  *resolution.activatedButton};
    if (resolution.lease.owner == applicationExitOwner_) {
      tui_application_exit::Transition transition =
          applicationExit_.handle(activation, activeTask);
      if (transition.handled) {
        apply(std::move(transition));
      }
    } else if (resolution.lease.owner == mediaTaskOwner_) {
      if (std::optional<tui_media_task_panel::DialogIntent> intent =
              mediaTaskDialogs_.handle(activation)) {
        publish(std::move(*intent));
      }
    } else if (resolution.lease.owner == audioFallbackOwner_) {
      if (std::optional<tui_playback_dialogs::AudioFallbackResolution>
              fallback = audioFallbackDialog_.handle(activation)) {
        events_.emplace_back(*fallback);
      }
    }
  }
  retire(resolution.lease);
}

bool Host::apply(tui_application_exit::Transition transition) {
  bool changed = false;
  if (transition.dismissDialog) {
    changed = dismissDialog(*transition.dismissDialog) || changed;
  }
  if (transition.openDialog) {
    const tui_dialog::DialogId dialog =
        open(applicationExitOwner_, std::move(*transition.openDialog));
    applicationExit_.opened(dialog);
    changed = true;
  }
  if (transition.intent) {
    publish(std::move(*transition.intent));
  }
  return changed;
}

void Host::publish(tui_application_exit::Intent intent) {
  std::visit(
      [this](auto&& value) {
        events_.emplace_back(std::forward<decltype(value)>(value));
      },
      std::move(intent));
}

void Host::publish(tui_media_task_panel::DialogIntent intent) {
  std::visit(
      [this](auto&& value) {
        events_.emplace_back(std::forward<decltype(value)>(value));
      },
      std::move(intent));
}

}  // namespace tui_shell_dialogs
