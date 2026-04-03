#include "playback_session_bootstrap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "playback_dialog.h"
#include "ui_helpers.h"

namespace {

void renderPreparingScreen(ConsoleScreen& screen, const std::filesystem::path& file,
                           const Style& baseStyle, const Style& accentStyle,
                           const Style& dimStyle,
                           const Style& progressEmptyStyle,
                           const Style& progressFrameStyle,
                           const Color& progressStart, const Color& progressEnd,
                           double progress) {
  screen.updateSize();
  int width = std::max(20, screen.width());
  int height = std::max(10, screen.height());
  screen.clear(baseStyle);
  std::string title = "Video: " + toUtf8String(file.filename());
  screen.writeText(0, 0, fitLine(title, width), accentStyle);
  std::string message = "Preparing video playback...";
  int msgLine = std::clamp(height / 2, 1, std::max(1, height - 2));
  int msgWidth = utf8CodepointCount(message);
  if (msgWidth >= width) {
    screen.writeText(0, msgLine, fitLine(message, width), dimStyle);
  } else {
    int msgX = (width - msgWidth) / 2;
    screen.writeText(msgX, msgLine, message, dimStyle);
  }
  int barWidth = std::min(32, width - 6);
  int barLine = msgLine + 1;
  if (barWidth >= 5 && barLine < height) {
    int barX = std::max(0, (width - (barWidth + 2)) / 2);
    screen.writeChar(barX, barLine, L'|', progressFrameStyle);
    auto barCells = renderProgressBarCells(progress, barWidth,
                                           progressEmptyStyle, progressStart,
                                           progressEnd);
    for (int i = 0; i < barWidth; ++i) {
      const auto& cell = barCells[static_cast<size_t>(i)];
      screen.writeChar(barX + 1 + i, barLine, cell.ch, cell.style);
    }
    screen.writeChar(barX + 1 + barWidth, barLine, L'|',
                     progressFrameStyle);
  }
  screen.draw();
}

bool showError(ConsoleInput& input, ConsoleScreen& screen,
               const Style& baseStyle, const Style& accentStyle,
               const Style& dimStyle, const std::string& message,
               const std::string& detail) {
  playback_dialog::showInfoDialog(input, screen, baseStyle, accentStyle,
                                  dimStyle, "Video error", message, detail,
                                  "");
  return true;
}

bool showAudioFallbackPrompt(ConsoleInput& input, ConsoleScreen& screen,
                             const Style& baseStyle, const Style& accentStyle,
                             const Style& dimStyle, const std::string& message,
                             const std::string& detail) {
  return playback_dialog::showConfirmDialog(
             input, screen, baseStyle, accentStyle, dimStyle, "Audio only?",
             message, detail, "") == playback_dialog::DialogResult::Confirmed;
}

}  // namespace

PlaybackSessionBootstrapOutcome bootstrapPlaybackSession(
    const std::filesystem::path& file, ConsoleInput& input,
    ConsoleScreen& screen, const Style& baseStyle, const Style& accentStyle,
    const Style& dimStyle, const Style& progressEmptyStyle,
    const Style& progressFrameStyle, const Color& progressStart,
    const Color& progressEnd, bool enableAudio, bool enableAscii,
    Player& player, bool* quitAppRequested) {
  if (quitAppRequested) {
    *quitAppRequested = false;
  }

  auto playerConfig = PlayerConfig{};
  playerConfig.file = file;
  playerConfig.enableAudio = enableAudio;
  playerConfig.allowDecoderScale = enableAscii;

  if (!player.open(playerConfig, nullptr)) {
    showError(input, screen, baseStyle, accentStyle, dimStyle,
              "Failed to open video.", "");
    return PlaybackSessionBootstrapOutcome::Handled;
  }

  constexpr auto kPrepRedrawInterval = std::chrono::milliseconds(120);
  constexpr double kPrepPulseSeconds = 1.6;
  auto initStart = std::chrono::steady_clock::now();
  auto lastInitDraw = std::chrono::steady_clock::time_point::min();
  bool running = true;
  while (running) {
    if (player.initDone()) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastInitDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - initStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double ratio = (phase <= (kPrepPulseSeconds * 0.5))
                         ? (phase / (kPrepPulseSeconds * 0.5))
                         : ((kPrepPulseSeconds - phase) /
                            (kPrepPulseSeconds * 0.5));
      renderPreparingScreen(screen, file, baseStyle, accentStyle, dimStyle,
                            progressEmptyStyle, progressFrameStyle,
                            progressStart, progressEnd, ratio);
      lastInitDraw = now;
    }

    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
          if (quitAppRequested) {
            *quitAppRequested = true;
          }
          running = false;
          break;
        }
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
            key.vk == VK_BACK) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                               FROM_LEFT_2ND_BUTTON_PRESSED |
                               FROM_LEFT_3RD_BUTTON_PRESSED |
                               FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mouse.buttonState & backMask) != 0) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastInitDraw = std::chrono::steady_clock::time_point::min();
      }
    }
  }

  if (!running) {
    player.close();
    return PlaybackSessionBootstrapOutcome::Handled;
  }

  if (!player.initOk()) {
    player.close();
    std::string initError = player.initError();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        showError(input, screen, baseStyle, accentStyle, dimStyle,
                  "No video stream found.",
                  "Audio playback is disabled.");
        return PlaybackSessionBootstrapOutcome::Handled;
      }
      bool playAudio = showAudioFallbackPrompt(
          input, screen, baseStyle, accentStyle, dimStyle,
          "No video stream found.",
          "This file can be played as audio only.");
      return playAudio ? PlaybackSessionBootstrapOutcome::PlayAudioOnly
                       : PlaybackSessionBootstrapOutcome::Handled;
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    showError(input, screen, baseStyle, accentStyle, dimStyle, initError, "");
    return PlaybackSessionBootstrapOutcome::Handled;
  }

  return PlaybackSessionBootstrapOutcome::ContinueVideo;
}
