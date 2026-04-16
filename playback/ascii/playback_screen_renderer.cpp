#include "playback_screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "ui_helpers.h"
#include "unicode_display_width.h"

namespace playback_screen_renderer {
namespace {

[[maybe_unused]] const char* playerStateLabel(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "Idle";
    case PlayerState::Opening:
      return "Opening";
    case PlayerState::Prefill:
      return "Prefill";
    case PlayerState::Priming:
      return "Priming";
    case PlayerState::Playing:
      return "Playing";
    case PlayerState::Paused:
      return "Paused";
    case PlayerState::Seeking:
      return "Seeking";
    case PlayerState::Draining:
      return "Draining";
    case PlayerState::Ended:
      return "Ended";
    case PlayerState::Error:
      return "Error";
    case PlayerState::Closing:
      return "Closing";
  }
  return "Unknown";
}

[[maybe_unused]] const char* clockSourceLabel(PlayerClockSource source) {
  switch (source) {
    case PlayerClockSource::None:
      return "none";
    case PlayerClockSource::Audio:
      return "audio";
    case PlayerClockSource::Video:
      return "video";
  }
  return "none";
}

}  // namespace

void renderPlaybackScreen(PlaybackScreenRenderInputs& inputs) {
  auto& screen = *inputs.screen;
  auto& videoWindow = *inputs.videoWindow;
  auto& player = *inputs.player;
  auto& subtitleManager = *inputs.subtitleManager;
  auto& gpuRenderer = *inputs.gpuRenderer;
  auto& frameCache = *inputs.frameCache;
  auto& art = *inputs.art;
  VideoFrame* frame = inputs.frame;
  const std::string& windowTitle = *inputs.windowTitle;
  const Style& baseStyle = *inputs.baseStyle;
  const Style& accentStyle = *inputs.accentStyle;
  const Style& dimStyle = *inputs.dimStyle;
  const Style& progressEmptyStyle = *inputs.progressEmptyStyle;
  const Style& progressFrameStyle = *inputs.progressFrameStyle;
  const Color& progressStart = *inputs.progressStart;
  const Color& progressEnd = *inputs.progressEnd;
  bool debugOverlay = inputs.debugOverlay;
  PlaybackRenderMode currentMode = inputs.currentMode;
  PlaybackSessionState playbackState = inputs.playbackState;
  bool enableAudio = inputs.enableAudio;
  bool audioOk = inputs.audioOk;
  bool audioStarting = inputs.audioStarting;
  bool windowActive = inputs.windowActive;
  bool hasSubtitles = inputs.hasSubtitles;
  bool allowAsciiCpuFallback = inputs.allowAsciiCpuFallback;
  bool useWindowPresenter = inputs.useWindowPresenter;
  bool overlayVisibleNow = inputs.overlayVisibleNow;
  bool clearHistory = inputs.clearHistory;
  bool frameChanged = inputs.frameChanged;
  bool localSeekRequested = inputs.localSeekRequested;
  double pendingSeekTargetSec = inputs.pendingSeekTargetSec;
  auto& enableSubtitlesShared = *inputs.enableSubtitlesShared;
  auto& windowLocalSeekRequested = *inputs.windowLocalSeekRequested;
  auto& overlayControlHover = *inputs.overlayControlHover;
  bool& renderFailed = *inputs.renderFailed;
  std::string& renderFailMessage = *inputs.renderFailMessage;
  std::string& renderFailDetail = *inputs.renderFailDetail;
  bool& haveFrame = *inputs.haveFrame;
  int& cachedWidth = *inputs.cachedWidth;
  int& cachedMaxHeight = *inputs.cachedMaxHeight;
  int& cachedFrameWidth = *inputs.cachedFrameWidth;
  int& cachedFrameHeight = *inputs.cachedFrameHeight;
  int& progressBarX = *inputs.progressBarX;
  int& progressBarY = *inputs.progressBarY;
  int& progressBarWidth = *inputs.progressBarWidth;
  const auto& warningSink = inputs.warningSink;
  const auto& timingSink = inputs.timingSink;
  screen.updateSize();
  int width = std::max(20, screen.width());
  int height = std::max(10, screen.height());
  if (!overlayVisibleNow) {
    overlayControlHover.store(-1, std::memory_order_relaxed);
  }
  std::string statusLine;
  if (!audioOk && !audioStarting) {
    statusLine = enableAudio ? "Audio unavailable" : "Audio disabled";
  }
  std::string subtitleText;
  std::string debugLine1;
  std::string debugLine2;
#if RADIOIFY_ENABLE_TIMING_LOG
  if (debugOverlay) {
    PlayerDebugInfo dbg = player.debugInfo();
    char buf1[256];
    char buf2[256];
    double masterSec = static_cast<double>(dbg.masterClockUs) / 1000000.0;
    double diffMs = static_cast<double>(dbg.lastDiffUs) / 1000.0;
    double delayMs = static_cast<double>(dbg.lastDelayUs) / 1000.0;
    std::snprintf(
        buf1, sizeof(buf1),
        "DBG state=%s serial=%d seek=%d qv=%zu master=%s %.3fs diff=%.1fms delay=%.1fms",
        playerStateLabel(dbg.state), dbg.currentSerial, dbg.pendingSeekSerial,
        dbg.videoQueueDepth, clockSourceLabel(dbg.masterSource), masterSec,
        diffMs, delayMs);
    std::snprintf(
        buf2, sizeof(buf2),
        "DBG audio ok=%d ready=%d fresh=%d starved=%d buf=%zuf rate=%u clock=%.3fs",
        dbg.audioOk ? 1 : 0, dbg.audioClockReady ? 1 : 0,
        dbg.audioClockFresh ? 1 : 0, dbg.audioStarved ? 1 : 0,
        dbg.audioBufferedFrames, dbg.audioSampleRate,
        static_cast<double>(dbg.audioClockUs) / 1000000.0);
    debugLine1 = buf1;
    debugLine2 = buf2;
  }
#else
  (void)debugOverlay;
#endif
  int headerLines = 0;
  if (!debugLine1.empty()) {
    headerLines += 1;
  }
  if (!debugLine2.empty()) {
    headerLines += 1;
  }
  if (!statusLine.empty()) {
    headerLines += 1;
  }
  const int footerLines = 0;
  int artTop = headerLines;
  int maxHeight = std::max(1, height - headerLines - footerLines);

  double currentSec = 0.0;
  double totalSec = -1.0;
  int64_t clockUs = player.currentUs();
  if (clockUs > 0) {
    currentSec = static_cast<double>(clockUs) / 1000000.0;
  }
  int64_t durUs = player.durationUs();
  if (durUs > 0) {
    totalSec = static_cast<double>(durUs) / 1000000.0;
  } else if (audioOk) {
    totalSec = audioGetTotalSec();
  }
  if (totalSec > 0.0) {
    currentSec = std::clamp(currentSec, 0.0, totalSec);
  }
  double displaySec = currentSec;
  bool seekingOverlay = player.isSeeking() || localSeekRequested;
  if (seekingOverlay && totalSec > 0.0 && std::isfinite(totalSec)) {
    displaySec = std::clamp(pendingSeekTargetSec, 0.0, totalSec);
  }
  const bool subtitlesEnabledNow =
      enableSubtitlesShared.load(std::memory_order_relaxed);
  subtitleText = playback_overlay::buildSubtitleText(
      subtitleManager, subtitlesEnabledNow, seekingOverlay, clockUs,
      hasSubtitles);

  const bool hasVideoStream =
      player.sourceWidth() > 0 && player.sourceHeight() > 0;
  bool waitingForAudio = audioOk && !audioStreamClockReady() && !audioIsFinished();
  bool audioStarved = audioOk && audioStreamStarved();
  bool waitingForVideo = hasVideoStream && !player.hasVideoFrame();
  bool isPaused = playbackState == PlaybackSessionState::Paused ||
                  player.state() == PlayerState::Paused;
  bool allowFrame = haveFrame && !useWindowPresenter;

  auto waitingLabel = [&]() -> std::string {
    if (playbackState == PlaybackSessionState::Ended) return "Ended";
    if (seekingOverlay) return "Seeking...";
    if (isPaused) return "Paused";
    if (player.state() == PlayerState::Opening) return "Opening...";
    if (player.state() == PlayerState::Prefill) return "Prefilling...";
    if (waitingForAudio) return "Waiting for audio...";
    if (audioStarved) return "Buffering audio...";
    if (waitingForVideo) return "Buffering video...";
    if (!hasVideoStream && audioOk) return "Audio playback";
    return "Waiting for video...";
  };

  bool sizeChanged = (width != cachedWidth || maxHeight != cachedMaxHeight ||
                      frame->width != cachedFrameWidth ||
                      frame->height != cachedFrameHeight);

  if (currentMode == PlaybackRenderMode::AsciiTerminal) {
    playback_frame_output::AsciiModePrepareInput asciiInput;
    asciiInput.allowFrame = allowFrame;
    asciiInput.clearHistory = clearHistory;
    asciiInput.frameChanged = frameChanged;
    asciiInput.sizeChanged = sizeChanged;
    asciiInput.allowAsciiCpuFallback = allowAsciiCpuFallback;
    asciiInput.width = width;
    asciiInput.maxHeight = maxHeight;
    asciiInput.computeAsciiOutputSize =
        playback_frame_output::computeAsciiOutputSize;
    asciiInput.frame = frame;
    asciiInput.art = &art;
    asciiInput.gpuRenderer = &gpuRenderer;
    asciiInput.frameCache = &frameCache;
    asciiInput.renderFailed = &renderFailed;
    asciiInput.renderFailMessage = &renderFailMessage;
    asciiInput.renderFailDetail = &renderFailDetail;
    asciiInput.haveFrame = &haveFrame;
    asciiInput.cachedWidth = &cachedWidth;
    asciiInput.cachedMaxHeight = &cachedMaxHeight;
    asciiInput.cachedFrameWidth = &cachedFrameWidth;
    asciiInput.cachedFrameHeight = &cachedFrameHeight;
    asciiInput.warningSink = warningSink;
    asciiInput.timingSink = timingSink;
    playback_frame_output::prepareAsciiModeFrame(asciiInput);
  } else {
    playback_frame_output::prepareNonAsciiModeFrame(
        allowFrame, width, maxHeight, frame->width, frame->height,
        &cachedWidth, &cachedMaxHeight, &cachedFrameWidth, &cachedFrameHeight,
        warningSink, &haveFrame);
  }

  screen.clear(baseStyle);
  int headerY = 0;
  if (!debugLine1.empty()) {
    screen.writeText(0, headerY++, fitLine(debugLine1, width), dimStyle);
  }
  if (!debugLine2.empty()) {
    screen.writeText(0, headerY++, fitLine(debugLine2, width), dimStyle);
  }
  if (!statusLine.empty()) {
    screen.writeText(0, headerY++, fitLine(statusLine, width), dimStyle);
  }

  if (currentMode == PlaybackRenderMode::AsciiTerminal) {
    playback_frame_output::renderAsciiModeContent(
        screen, art, width, height, maxHeight, artTop, waitingLabel(),
        allowFrame, baseStyle, overlayVisibleNow, subtitleText, accentStyle,
        dimStyle);
  } else {
    playback_frame_output::renderNonAsciiModeContent(
        screen, windowActive, allowFrame, width, artTop, maxHeight, frame,
        videoWindow.GetWidth(), videoWindow.GetHeight(), dimStyle);
  }

  progressBarX = -1;
  progressBarY = -1;
  progressBarWidth = 0;
  const bool audioFinishedNow = audioOk && audioIsFinished();
  const bool pausedNow = playbackState == PlaybackSessionState::Paused ||
                         player.state() == PlayerState::Paused;
  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = windowTitle;
  overlayInputs.audioOk = audioOk;
  overlayInputs.audioSupports50HzToggle = audioOk && audioSupports50HzToggle();
  overlayInputs.radioEnabled = audioIsRadioEnabled();
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.canCycleAudioTracks = audioOk && player.canCycleAudioTracks();
  overlayInputs.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  overlayInputs.subtitleManager = &subtitleManager;
  overlayInputs.hasSubtitles = hasSubtitles;
  overlayInputs.subtitlesEnabled = subtitlesEnabledNow;
  overlayInputs.subtitleClockUs = clockUs;
  overlayInputs.seekingOverlay =
      player.isSeeking() ||
      windowLocalSeekRequested.load(std::memory_order_relaxed);
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  overlayInputs.overlayVisible = overlayVisibleNow;
  overlayInputs.paused = pausedNow;
  overlayInputs.audioFinished = audioFinishedNow;
  overlayInputs.pictureInPictureAvailable = videoWindow.IsOpen();
  overlayInputs.pictureInPictureActive =
      overlayInputs.pictureInPictureAvailable &&
      videoWindow.IsPictureInPicture();
  overlayInputs.subtitleRenderError = videoWindow.GetSubtitleRenderError();
  overlayInputs.screenWidth = width;
  overlayInputs.screenHeight = height;
  overlayInputs.windowWidth = videoWindow.IsOpen() ? videoWindow.GetWidth() : 0;
  overlayInputs.windowHeight = videoWindow.IsOpen() ? videoWindow.GetHeight() : 0;
  overlayInputs.artTop = artTop;
  overlayInputs.progressBarX = progressBarX;
  overlayInputs.progressBarY = progressBarY;
  overlayInputs.progressBarWidth = progressBarWidth;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);
  if (overlayState.overlayVisible) {
    int barLine = height - 1;
    int suffixLine = barLine - 1;
    int controlsLine = suffixLine - 1;
    int titleLine = controlsLine - 1;
    if (controlsLine >= artTop && controlsLine >= 0) {
      std::vector<playback_overlay::OverlayControlSpec> controls =
          playback_overlay::buildOverlayControlSpecs(
              overlayState, overlayControlHover.load(std::memory_order_relaxed));
      for (size_t i = 0; i < controls.size(); ++i) {
        const auto& spec = controls[i];
        int x = 1 + spec.charStart;
        if (x >= width) break;
        int avail = width - x;
        if (avail <= 0) break;
        std::string text = spec.renderText;
        if (utf8DisplayWidth(text) > avail) {
          text = utf8TakeDisplayWidth(text, avail);
        }
        Style style = spec.active ? accentStyle : baseStyle;
        bool hovered =
            static_cast<int>(i) ==
            overlayControlHover.load(std::memory_order_relaxed);
        if (hovered) {
          style = {style.bg, style.fg};
        }
        screen.writeText(x, controlsLine, text, style);
      }
    }
    if (titleLine >= artTop && titleLine >= 0) {
      std::string titleLineText =
          " " + playback_overlay::buildWindowOverlayTopLine(overlayState);
      screen.writeText(0, titleLine, fitLine(titleLineText, width),
                       accentStyle);
    }

    std::string suffix =
        playback_overlay::buildWindowOverlayProgressSuffix(overlayState);
    int barWidth = std::max(5, width - 2);
    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }
    progressBarX = 1;
    progressBarY = barLine;
    progressBarWidth = barWidth;
    screen.writeChar(0, barLine, L'|', progressFrameStyle);
    auto barCells = renderProgressBarCells(ratio, barWidth, progressEmptyStyle,
                                           progressStart, progressEnd);
    for (int i = 0; i < barWidth; ++i) {
      const auto& cell = barCells[static_cast<size_t>(i)];
      screen.writeChar(1 + i, barLine, cell.ch, cell.style);
    }
    screen.writeChar(1 + barWidth, barLine, L'|', progressFrameStyle);
    if (!suffix.empty() && suffixLine >= artTop && suffixLine >= 0) {
      std::string suffixFit = fitLine(suffix, width);
      int suffixWidth = utf8DisplayWidth(suffixFit);
      int suffixX = std::max(0, width - suffixWidth);
      screen.writeText(suffixX, suffixLine, suffixFit, baseStyle);
    }
  }

  screen.draw();
}

}  // namespace playback_screen_renderer
