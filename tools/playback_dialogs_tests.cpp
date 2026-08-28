#include <cstdlib>
#include <iostream>
#include <string>

#include "tui/ui/playback_dialogs.h"

namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "playback_dialogs_tests: " << message << '\n';
  return false;
}

bool containsText(const tui_dialog::Content &content,
                  const std::string &needle) {
  for (const tui_dialog::TextBlock &block : content.text) {
    if (block.text.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

} // namespace

int main() {
  using namespace tui_playback_dialogs;
  bool ok = true;

  const tui_dialog::Content audioFailure =
      audioPlaybackFailure("example.flac", "Decoder initialization failed.");
  ok &= expect(audioFailure.title == "Playback Error" &&
                   containsText(audioFailure, "Decoder initialization") &&
                   containsText(audioFailure, "example.flac") &&
                   audioFailure.buttons.size() == 1,
               "audio failures must retain their reason and source");

  const tui_dialog::Content psfFailure =
      audioPlaybackFailure("game.psf2", "Could not find hebios.bin");
  ok &= expect(psfFailure.title == "PSF2 BIOS Required" &&
                   containsText(psfFailure, "RADIOIFY_PSF_BIOS"),
               "PSF2 failures must retain their actionable BIOS guidance");

  tui_media_activation::AudioFallbackRequest request;
  request.id = {42};
  request.file = "music-only.mkv";
  request.reason = {"No video stream found.",
                    "This file can be played as audio only."};
  const tui_dialog::Content prompt = audioFallback(request);
  ok &=
      expect(prompt.title == "Play audio only?" && prompt.buttons.size() == 2 &&
                 prompt.initiallySelectedButton == kCancelButton &&
                 containsText(prompt, "music-only.mkv"),
             "audio fallback must be an explicit safe-default decision");

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
                 dismissed->decision == tui_media_activation::DecisionId{84} &&
                 !dismissed->playAudio,
             "closing or replacing the prompt must decline hidden work");

  session.opened({10}, {126});
  const std::optional<AudioFallbackResolution> cancelled =
      session.handle({{10}, kCancelButton});
  ok &= expect(cancelled && !cancelled->playAudio,
               "the visible Cancel action must decline audio fallback");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
