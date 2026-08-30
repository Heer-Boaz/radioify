#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "tui/ui/playback_dialogs.h"

namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "playback_dialogs_tests: " << message << '\n';
  return false;
}

} // namespace

int main() {
  using namespace tui_playback_dialogs;
  bool ok = true;

  application_playback::AudioFallbackRequest request;
  request.id = {42};
  request.file = "music-only.mkv";
  request.reason = {"No video stream found.",
                    "This file can be played as audio only."};
  const tui_dialog::Content prompt = audioFallback(request);
  const bool hasPlayAction =
      std::any_of(prompt.buttons.begin(), prompt.buttons.end(),
                  [](const tui_dialog::Button &button) {
                    return button.id == kPlayAudioButton;
                  });
  const bool hasCancelAction =
      std::any_of(prompt.buttons.begin(), prompt.buttons.end(),
                  [](const tui_dialog::Button &button) {
                    return button.id == kCancelButton;
                  });
  ok &= expect(hasPlayAction && hasCancelAction &&
                   prompt.initiallySelectedButton == kCancelButton,
               "audio fallback must expose both outcomes and default to the "
               "non-activating choice");

  AudioFallbackSession session;
  session.opened({7}, request.id);
  ok &= expect(!session.handle({{8}, kPlayAudioButton}),
               "an unrelated dialog must not resolve the fallback");
  const std::optional<AudioFallbackResolution> accepted =
      session.handle({{7}, kPlayAudioButton});
  ok &= expect(accepted && accepted->decision == request.id &&
                   accepted->playAudio,
               "Play audio must resolve the exact activation request");
  ok &= expect(!session.dismissed({7}),
               "a resolved prompt must not resolve twice");

  session.opened({9}, {84});
  const std::optional<AudioFallbackResolution> dismissed =
      session.dismissed({9});
  ok &=
      expect(dismissed &&
                 dismissed->decision ==
                     application_playback::AudioFallbackDecisionId{84} &&
                 !dismissed->playAudio,
             "closing or replacing the prompt must decline hidden work");

  session.opened({10}, {126});
  const std::optional<AudioFallbackResolution> cancelled =
      session.handle({{10}, kCancelButton});
  ok &= expect(cancelled && !cancelled->playAudio,
               "the visible Cancel action must decline audio fallback");

  session.opened({11}, {168});
  ok &= expect(!session.revoke({169}) && session.revoke({168}) ==
                                              tui_dialog::DialogId{11} &&
                   !session.handle({{11}, kPlayAudioButton}),
               "only a matching domain revocation may retire a pending "
               "fallback dialog");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
