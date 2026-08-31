#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <variant>

#include "tui/ui/shell_dialog_host.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "shell_dialog_host_tests: " << message << '\n';
  return false;
}

InputEvent keyEvent(WORD key) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.pressKind = KeyPressKind::Initial;
  event.key.repeatCount = 1;
  return event;
}

MediaTaskCardModel activeCard(media_processing::TaskId id,
                              bool cancellable = true,
                              bool cancelling = false) {
  MediaTaskCardModel task;
  task.taskId = id;
  task.operation = playback_media_processing::Operation::AudioSeparation;
  task.sourceName = "source.mp4";
  task.cancellable = cancellable;
  task.cancelling = cancelling;
  return task;
}

playback_media_processing::Activity activeActivity(
    playback_media_processing::TaskId id, bool cancellable = true,
    bool cancelling = false) {
  playback_media_processing::Activity task;
  task.taskId = id;
  task.operation = playback_media_processing::Operation::AudioSeparation;
  task.sourceFile = "source.mp4";
  task.cancellable = cancellable;
  task.cancelling = cancelling;
  return task;
}

tui_dialog::Content informationDialog() {
  tui_dialog::Content content;
  content.buttons.push_back({0, "Close"});
  return content;
}

application_playback::AudioFallbackRequest fallbackRequest(
    application_playback::AudioFallbackDecisionId id) {
  application_playback::AudioFallbackRequest request;
  request.id = id;
  request.file = "audio-only.mkv";
  return request;
}

const shell_overlay_stack::Bounds kBounds{60, 16, 1};

}  // namespace

int main() {
  using namespace tui_shell_dialogs;
  bool ok = true;
  const shell_command_catalog::Catalog catalog =
      shell_command_catalog::build({});

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const auto firstId = application_playback::AudioFallbackDecisionId{41};
    const auto secondId = application_playback::AudioFallbackDecisionId{42};

    host.showAudioFallback(fallbackRequest(firstId));
    host.showAudioFallback(fallbackRequest(secondId));
    std::vector<Event> events = host.drainEvents();
    const auto* displaced =
        events.size() == 1
            ? std::get_if<tui_playback_dialogs::AudioFallbackResolution>(
                  &events.front())
            : nullptr;
    ok &= expect(displaced && displaced->decision == firstId &&
                     !displaced->playAudio && overlays.inputModal(),
                 "replacing a fallback decision must decline only the "
                 "displaced request while preserving the new prompt");

    const Interaction interaction =
        host.handle(keyEvent(VK_RETURN), kBounds, catalog, std::nullopt);
    events = host.drainEvents();
    const auto* cancelled =
        events.size() == 1
            ? std::get_if<tui_playback_dialogs::AudioFallbackResolution>(
                  &events.front())
            : nullptr;
    ok &= expect(interaction.consumed && interaction.changed && cancelled &&
                     cancelled->decision == secondId && !cancelled->playAudio &&
                     !overlays.active(),
                 "the fallback safe-default action must resolve the exact "
                 "visible decision and retire its modal");
    ok &= expect(host.drainEvents().empty(),
                 "a resolved fallback decision must publish exactly once");

    const auto acceptedId = application_playback::AudioFallbackDecisionId{43};
    host.showAudioFallback(fallbackRequest(acceptedId));
    host.handle(keyEvent(VK_LEFT), kBounds, catalog, std::nullopt);
    host.handle(keyEvent(VK_RETURN), kBounds, catalog, std::nullopt);
    events = host.drainEvents();
    const auto* accepted =
        events.size() == 1
            ? std::get_if<tui_playback_dialogs::AudioFallbackResolution>(
                  &events.front())
            : nullptr;
    ok &= expect(accepted && accepted->decision == acceptedId &&
                     accepted->playAudio && !overlays.active(),
                 "the affirmative fallback path must resolve only the "
                 "decision represented by the visible prompt");
  }

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const auto decision = application_playback::AudioFallbackDecisionId{84};
    host.showAudioFallback(fallbackRequest(decision));
    ok &= expect(!host.revokeAudioFallback(
                     application_playback::AudioFallbackDecisionId{85}) &&
                     overlays.inputModal() && host.drainEvents().empty(),
                 "an unrelated fallback revocation must leave the current "
                 "decision pending");
    ok &= expect(host.revokeAudioFallback(decision) && !overlays.active() &&
                     host.drainEvents().empty(),
                 "a matching domain revocation must retire its prompt "
                 "without publishing a user decision");

    const auto dismissedDecision =
        application_playback::AudioFallbackDecisionId{126};
    host.showAudioFallback(fallbackRequest(dismissedDecision));
    ok &= expect(host.dismissOverlays() && !overlays.active(),
                 "generic overlay dismissal must retire the active dialog");
    const std::vector<Event> dismissedEvents = host.drainEvents();
    const auto* dismissed =
        dismissedEvents.size() == 1
            ? std::get_if<tui_playback_dialogs::AudioFallbackResolution>(
                  &dismissedEvents.front())
            : nullptr;
    ok &= expect(dismissed && dismissed->decision == dismissedDecision &&
                     !dismissed->playAudio,
                 "dismissing a fallback prompt must safely decline its "
                 "hidden activation request");
  }

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const MediaTaskCardModel running =
        activeCard(media_processing::TaskId{200});
    host.showMediaTask(
        tui_media_task_panel::cancellationDialogRequest(running));
    host.handle(keyEvent(VK_LEFT), kBounds, catalog, std::nullopt);
    const Interaction activation =
        host.handle(keyEvent(VK_RETURN), kBounds, catalog, std::nullopt);
    const std::vector<Event> events = host.drainEvents();
    const auto* cancellation =
        events.size() == 1
            ? std::get_if<tui_media_task_panel::CancelTask>(&events.front())
            : nullptr;
    ok &=
        expect(activation.consumed && activation.changed && cancellation &&
                   cancellation->taskId == running.taskId && !overlays.active(),
               "media-task confirmation must publish the exact task "
               "identity selected through the live dialog workflow");

    host.showMediaTask(
        tui_media_task_panel::cancellationDialogRequest(running));
    const MediaTaskCardModel replacement =
        activeCard(media_processing::TaskId{201});
    ok &= expect(host.synchronizeMediaTask(replacement, std::nullopt, {}) &&
                     !overlays.active() && host.drainEvents().empty(),
                 "a task replacement must invalidate its predecessor's "
                 "confirmation without leaking a cancellation intent");

    const std::filesystem::path setupSource = "setup-source.mp4";
    host.showMediaTask(
        tui_media_task_panel::audioSeparationSetupDialogRequest(
            playback_media_processing::AudioSeparationSetupRequest{
                setupSource}));
    ok &= expect(
        host.synchronizeMediaTask(
            std::nullopt, std::nullopt,
            [&](const std::filesystem::path& source) {
              return source != setupSource;
            }) &&
            !overlays.active() && host.drainEvents().empty(),
        "current availability must retire an obsolete setup dialog without "
        "publishing setup intent");
  }

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const playback_media_processing::Activity running =
        activeActivity(playback_media_processing::TaskId{300});
    host.requestApplicationExit(running);
    host.handle(keyEvent(VK_LEFT), kBounds, catalog, running);
    host.handle(keyEvent(VK_RETURN), kBounds, catalog, running);
    std::vector<Event> events = host.drainEvents();
    const auto* cancel =
        events.size() == 1
            ? std::get_if<tui_application_exit::CancelTask>(&events.front())
            : nullptr;
    ok &=
        expect(cancel && cancel->taskId == running.taskId && !overlays.active(),
               "quit confirmation must hand cancellation to the exact "
               "active task owner before quitting");

    const playback_media_processing::Activity cancelling =
        activeActivity(running.taskId, false, true);
    host.resolveApplicationExitCancellation(running.taskId, true, cancelling);
    ok &= expect(overlays.inputModal() && host.drainEvents().empty(),
                 "accepted cancellation must keep the asynchronous exit "
                 "workflow visible while the task still owns work");
    host.synchronizeApplicationExit(cancelling);
    ok &= expect(overlays.inputModal() && host.drainEvents().empty(),
                 "an in-flight cancellation must not publish premature quit");
    host.synchronizeApplicationExit(std::nullopt);
    events = host.drainEvents();
    ok &= expect(events.size() == 1 &&
                     std::holds_alternative<tui_application_exit::QuitNow>(
                         events.front()) &&
                     !overlays.active(),
                 "application exit must publish only after the represented "
                 "background task has ended");
  }

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const playback_media_processing::Activity running =
        activeActivity(playback_media_processing::TaskId{400});
    host.requestApplicationExit(running);
    host.showInformation(informationDialog());
    ok &= expect(overlays.inputModal() && host.drainEvents().empty(),
                 "replacing quit confirmation with information must cancel "
                 "only the pending exit interaction");
    host.synchronizeApplicationExit(std::nullopt);
    ok &= expect(host.drainEvents().empty(),
                 "a retired exit dialog must never produce a delayed quit");
    host.handle(keyEvent(VK_RETURN), kBounds, catalog, std::nullopt);
    ok &= expect(!overlays.active() && host.drainEvents().empty(),
                 "an information dialog must close without a domain event");

    host.requestApplicationExit(std::nullopt);
    const std::vector<Event> immediate = host.drainEvents();
    ok &= expect(immediate.size() == 1 &&
                     std::holds_alternative<tui_application_exit::QuitNow>(
                         immediate.front()) &&
                     !overlays.active(),
                 "exit without owned background work must remain immediate");
  }

  {
    shell_overlay_stack::Model overlays;
    Host host(overlays);
    const playback_media_processing::Activity first =
        activeActivity(playback_media_processing::TaskId{500});
    const playback_media_processing::Activity replacement =
        activeActivity(playback_media_processing::TaskId{501});
    host.requestApplicationExit(first);
    host.handle(keyEvent(VK_LEFT), kBounds, catalog, first);
    host.handle(keyEvent(VK_RETURN), kBounds, catalog, first);
    const std::vector<Event> firstEvents = host.drainEvents();
    const auto* firstCancellation =
        firstEvents.size() == 1 ? std::get_if<tui_application_exit::CancelTask>(
                                      &firstEvents.front())
                                : nullptr;
    ok &= expect(firstCancellation && firstCancellation->taskId == first.taskId,
                 "the initial exit decision must remain correlated with its "
                 "original task until the owner resolves cancellation");

    host.resolveApplicationExitCancellation(first.taskId, false, replacement);
    host.handle(keyEvent(VK_LEFT), kBounds, catalog, replacement);
    host.handle(keyEvent(VK_RETURN), kBounds, catalog, replacement);
    const std::vector<Event> replacementEvents = host.drainEvents();
    const auto* replacementCancellation =
        replacementEvents.size() == 1
            ? std::get_if<tui_application_exit::CancelTask>(
                  &replacementEvents.front())
            : nullptr;
    ok &= expect(replacementCancellation &&
                     replacementCancellation->taskId == replacement.taskId &&
                     !overlays.active(),
                 "a stale cancellation result must reopen the workflow for "
                 "the replacement task instead of targeting its predecessor");
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
