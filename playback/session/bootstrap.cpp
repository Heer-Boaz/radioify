#include "bootstrap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "consoleinput.h"
#include "consolescreen.h"
#include "playback/session/bootstrap_input.h"
#include "playback/video/player.h"
#include "runtime_helpers.h"
#include "ui_helpers.h"

namespace {

double pulseProgress(std::chrono::steady_clock::time_point initStart,
                     std::chrono::steady_clock::time_point now) {
  constexpr double kPrepPulseSeconds = 1.6;
  double elapsed = std::chrono::duration<double>(now - initStart).count();
  double phase = std::fmod(elapsed, kPrepPulseSeconds);
  return (phase <= (kPrepPulseSeconds * 0.5))
             ? (phase / (kPrepPulseSeconds * 0.5))
             : ((kPrepPulseSeconds - phase) / (kPrepPulseSeconds * 0.5));
}

void renderPreparingScreen(
    ConsoleScreen& screen, const std::filesystem::path& file,
    const Style& baseStyle, const Style& accentStyle, const Style& dimStyle,
    const Style& progressEmptyStyle, const Style& progressFrameStyle,
    const Color& progressStart, const Color& progressEnd, double progress) {
  screen.updateSize();
  int width = std::max(20, screen.width());
  int height = std::max(10, screen.height());
  screen.clear(baseStyle);
  std::string title = "Video: " + toUtf8String(file.filename());
  screen.writeText(0, 0, fitLine(title, width), accentStyle);
  std::string message = "Preparing video playback...";
  int msgLine = std::clamp(height / 2, 1, std::max(1, height - 2));
  int msgWidth = utf8DisplayWidth(message);
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

}  // namespace

struct PlaybackSessionBootstrap::Impl {
  explicit Impl(Args args)
      : file(args.file),
        input(args.input),
        screen(args.screen),
        baseStyle(args.baseStyle),
        accentStyle(args.accentStyle),
        dimStyle(args.dimStyle),
        progressEmptyStyle(args.progressEmptyStyle),
        progressFrameStyle(args.progressFrameStyle),
        progressStart(args.progressStart),
        progressEnd(args.progressEnd),
        enableAudio(args.enableAudio),
        enableAscii(args.enableAscii),
        player(args.player) {}

  void requestQuit() { quitApplicationRequested = true; }

  std::optional<playback_session::Problem> openPlayer() {
    auto playerConfig = PlayerConfig{};
    playerConfig.file = file;
    playerConfig.enableAudio = enableAudio;
    playerConfig.allowDecoderScale = enableAscii;

    if (player.open(playerConfig, nullptr)) {
      return std::nullopt;
    }

    return playback_session::Problem{"Failed to open video.", {}};
  }

  void drawPreparingFrame(std::chrono::steady_clock::time_point initStart,
                          std::chrono::steady_clock::time_point now) {
    renderPreparingScreen(screen, file, baseStyle, accentStyle, dimStyle,
                          progressEmptyStyle, progressFrameStyle, progressStart,
                          progressEnd, pulseProgress(initStart, now));
  }

  bool handleMouseCancel(const MouseEvent& mouse) const {
    return mouse.kind == MouseEventKind::Press &&
           (mouse.button == MouseButton::Right ||
            mouse.button == MouseButton::Middle);
  }

  bool waitForInitialization() {
    constexpr auto kPrepRedrawInterval = std::chrono::milliseconds(120);
    auto initStart = std::chrono::steady_clock::now();
    auto lastInitDraw = std::chrono::steady_clock::time_point::min();
    while (!player.initDone()) {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastInitDraw >= kPrepRedrawInterval) {
        drawPreparingFrame(initStart, now);
        lastInitDraw = now;
      }

      InputEvent ev{};
      while (input.poll(ev)) {
        if (const auto action = playback_session_bootstrap_input::resolve(ev)) {
          using BootstrapAction = playback_session_bootstrap_input::Action;
          if (*action == BootstrapAction::QuitApplication) {
            requestQuit();
          }
          return false;
        }
        if (ev.type == InputEvent::Type::Mouse && handleMouseCancel(ev.mouse)) {
          return false;
        }
        if (ev.type == InputEvent::Type::Resize) {
          lastInitDraw = std::chrono::steady_clock::time_point::min();
        }
      }
    }
    return true;
  }

  playback_session::OpenOutcome handleInitFailure() {
    player.close();
    std::string initError = player.initError();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        return playback_session::OpenFailure{{
            "No video stream found.", "Audio playback is disabled."}};
      }
      return playback_session::OpenAudioFallback{{
          "No video stream found.",
          "This file can be played as audio only."}};
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    return playback_session::OpenFailure{{std::move(initError), {}}};
  }

  playback_session::OpenOutcome run() {
    if (std::optional<playback_session::Problem> failure = openPlayer()) {
      return playback_session::OpenFailure{std::move(*failure)};
    }
    if (!waitForInitialization()) {
      player.close();
      return quitApplicationRequested
                 ? playback_session::OpenOutcome(
                       playback_session::OpenQuitApplication{})
                 : playback_session::OpenOutcome(
                       playback_session::OpenCancelled{});
    }
    if (!player.initOk()) {
      return handleInitFailure();
    }
    return playback_session::OpenReady{};
  }

  const std::filesystem::path& file;
  ConsoleInput& input;
  ConsoleScreen& screen;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  const bool enableAudio;
  const bool enableAscii;
  Player& player;
  bool quitApplicationRequested = false;
};

PlaybackSessionBootstrap::PlaybackSessionBootstrap(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackSessionBootstrap::~PlaybackSessionBootstrap() = default;

PlaybackSessionBootstrap::PlaybackSessionBootstrap(
    PlaybackSessionBootstrap&&) noexcept = default;

PlaybackSessionBootstrap& PlaybackSessionBootstrap::operator=(
    PlaybackSessionBootstrap&&) noexcept = default;

playback_session::OpenOutcome PlaybackSessionBootstrap::run() {
  return impl_->run();
}
