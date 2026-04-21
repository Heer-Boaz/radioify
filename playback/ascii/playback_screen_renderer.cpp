#include "playback_screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "audioplayback.h"
#include "playback_ascii_subtitles.h"
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

std::pair<int, int> frameDisplaySize(const VideoFrame* frame) {
  if (!frame) {
    return {0, 0};
  }
  if ((frame->rotationQuarterTurns & 1) != 0) {
    return {frame->height, frame->width};
  }
  return {frame->width, frame->height};
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
  bool canPlayPrevious = inputs.canPlayPrevious;
  bool canPlayNext = inputs.canPlayNext;
  bool windowActive = inputs.windowActive;
  bool hasSubtitles = inputs.hasSubtitles;
  bool allowAsciiCpuFallback = inputs.allowAsciiCpuFallback;
  bool useWindowPresenter = inputs.useWindowPresenter;
  bool overlayVisibleNow = inputs.overlayVisibleNow;
  bool clearHistory = inputs.clearHistory;
  bool frameChanged = inputs.frameChanged;
  bool frameAvailable = inputs.frameAvailable;
  bool localSeekRequested = inputs.localSeekRequested;
  double cellPixelWidth = inputs.cellPixelWidth;
  double cellPixelHeight = inputs.cellPixelHeight;
  const std::string& cellPixelSourceLabel = inputs.cellPixelSourceLabel;
  double pendingSeekTargetSec = inputs.pendingSeekTargetSec;
  auto& enableSubtitlesShared = *inputs.enableSubtitlesShared;
  auto& windowLocalSeekRequested = *inputs.windowLocalSeekRequested;
  auto& overlayControlHover = *inputs.overlayControlHover;
  playback_frame_output::FrameOutputState& frameOutput =
      *inputs.frameOutputState;
  const auto& warningSink = inputs.warningSink;
  const auto& timingSink = inputs.timingSink;
  screen.updateSize();
  int width = screen.width();
  int height = screen.height();
  if (!overlayVisibleNow) {
    overlayControlHover.store(-1, std::memory_order_relaxed);
  }
  std::string statusLine;
  if (!audioOk && !audioStarting) {
    statusLine = enableAudio ? "Audio unavailable" : "Audio disabled";
  }
  auto [frameDisplayW, frameDisplayH] = frameDisplaySize(frame);
  int layoutSourceW = player.sourceWidth();
  int layoutSourceH = player.sourceHeight();
  const char* layoutSourceKind = "player";
  if (layoutSourceW <= 0 || layoutSourceH <= 0) {
    layoutSourceW = frameDisplayW;
    layoutSourceH = frameDisplayH;
    layoutSourceKind = "frame";
  }
  std::vector<std::string> debugLines;
  if (debugOverlay && currentMode == PlaybackRenderMode::AsciiTerminal) {
    char buf[512];
    const char* cellSource =
        cellPixelSourceLabel.empty() ? "unknown" : cellPixelSourceLabel.c_str();
    std::snprintf(buf, sizeof(buf),
                  "DBG cell=%.2fx%.2f/%s cols=%d rows=%d",
                  cellPixelWidth, cellPixelHeight, cellSource, width, height);
    debugLines.emplace_back(buf);

    int plannedDebugLineCount = 2;
#if RADIOIFY_ENABLE_TIMING_LOG
    plannedDebugLineCount += 2;
#endif
    const int plannedStatusLines = statusLine.empty() ? 0 : 1;
    const int plannedMaxHeight =
        height - plannedDebugLineCount - plannedStatusLines;
    int plannedArtW = 0;
    int plannedArtH = 0;
    if (layoutSourceW > 0 && layoutSourceH > 0 && plannedMaxHeight > 0) {
      auto plannedArt = playback_frame_output::computeAsciiOutputSize(
          width, plannedMaxHeight, layoutSourceW, layoutSourceH,
          cellPixelWidth, cellPixelHeight);
      plannedArtW = plannedArt.first;
      plannedArtH = plannedArt.second;
    }
    const double physW = plannedArtW * cellPixelWidth;
    const double physH = plannedArtH * cellPixelHeight;
    const double physAspect = physH > 0.0 ? physW / physH : 0.0;
    const double sourceAspect =
        layoutSourceH > 0
            ? static_cast<double>(layoutSourceW) /
                  static_cast<double>(layoutSourceH)
            : 0.0;
    char buf2[256];
    std::snprintf(
        buf2, sizeof(buf2),
        "DBG ascii src=%dx%d(%s) frame=%dx%d r=%d art=%dx%d phys=%.0fx%.0f asp=%.3f srcasp=%.3f path=%s",
        layoutSourceW, layoutSourceH, layoutSourceKind, frameDisplayW,
        frameDisplayH, frame ? frame->rotationQuarterTurns : 0, plannedArtW,
        plannedArtH, physW, physH, physAspect, sourceAspect,
        frameOutput.lastRenderPath.empty() ? "none"
                                           : frameOutput.lastRenderPath.c_str());
    debugLines.emplace_back(buf2);
  }
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
    debugLines.emplace_back(buf1);
    debugLines.emplace_back(buf2);
  }
#endif
  int headerLines = static_cast<int>(debugLines.size());
  if (!statusLine.empty()) {
    headerLines += 1;
  }
  const int footerLines = 0;
  int artTop = headerLines;
  int maxHeight = height - headerLines - footerLines;
  int asciiArtTop = artTop;

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
  const bool hasVideoStream =
      player.sourceWidth() > 0 && player.sourceHeight() > 0;
  bool waitingForAudio = audioOk && !audioStreamClockReady() && !audioIsFinished();
  bool audioStarved = audioOk && audioStreamStarved();
  bool waitingForVideo = hasVideoStream && !player.hasVideoFrame();
  bool isPaused = playbackState == PlaybackSessionState::Paused ||
                  player.state() == PlayerState::Paused;
  frameOutput.haveFrame = frameAvailable;
  bool allowFrame = frameOutput.haveFrame && !useWindowPresenter;

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

  bool sizeChanged =
      (width != frameOutput.cachedWidth ||
       maxHeight != frameOutput.cachedMaxHeight ||
       frame->width != frameOutput.cachedFrameWidth ||
       frame->height != frameOutput.cachedFrameHeight ||
       layoutSourceW != frameOutput.cachedLayoutSourceWidth ||
       layoutSourceH != frameOutput.cachedLayoutSourceHeight ||
       std::abs(cellPixelWidth - frameOutput.cachedCellPixelWidth) > 0.01 ||
       std::abs(cellPixelHeight - frameOutput.cachedCellPixelHeight) > 0.01);

  if (currentMode == PlaybackRenderMode::AsciiTerminal) {
    playback_frame_output::AsciiModePrepareInput asciiInput;
    asciiInput.allowFrame = allowFrame;
    asciiInput.clearHistory = clearHistory;
    asciiInput.frameChanged = frameChanged;
    asciiInput.sizeChanged = sizeChanged;
    asciiInput.allowAsciiCpuFallback = allowAsciiCpuFallback;
    asciiInput.width = width;
    asciiInput.maxHeight = maxHeight;
    asciiInput.cellPixelWidth = cellPixelWidth;
    asciiInput.cellPixelHeight = cellPixelHeight;
    asciiInput.sourceWidth = layoutSourceW;
    asciiInput.sourceHeight = layoutSourceH;
    asciiInput.computeAsciiOutputSize =
        playback_frame_output::computeAsciiOutputSize;
    asciiInput.frame = frame;
    asciiInput.art = &art;
    asciiInput.gpuRenderer = &gpuRenderer;
    asciiInput.frameCache = &frameCache;
    asciiInput.state = &frameOutput;
    asciiInput.warningSink = warningSink;
    asciiInput.timingSink = timingSink;
    playback_frame_output::prepareAsciiModeFrame(asciiInput);
  } else {
    playback_frame_output::prepareNonAsciiModeFrame(
        allowFrame, width, maxHeight, frame->width, frame->height,
        frameOutput, warningSink);
  }

  if (currentMode == PlaybackRenderMode::AsciiTerminal && allowFrame &&
      art.width > 0 && art.height > 0) {
    const int visibleArtHeight = std::min(art.height, maxHeight);
    asciiArtTop = playback_frame_output::centerContentTop(
        artTop, maxHeight, visibleArtHeight);
  }

  frameOutput.progressBarX = -1;
  frameOutput.progressBarY = -1;
  frameOutput.progressBarWidth = 0;
  const bool audioFinishedNow = audioOk && audioIsFinished();
  const bool pausedNow = playbackState == PlaybackSessionState::Paused ||
                         player.state() == PlayerState::Paused;
  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = windowTitle;
  overlayInputs.audioOk = audioOk;
  overlayInputs.playPauseAvailable =
      playbackState == PlaybackSessionState::Active ||
      playbackState == PlaybackSessionState::Paused;
  overlayInputs.audioSupports50HzToggle = audioOk && audioSupports50HzToggle();
  overlayInputs.canPlayPrevious = canPlayPrevious;
  overlayInputs.canPlayNext = canPlayNext;
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
  overlayInputs.pictureInPictureAvailable = true;
  overlayInputs.pictureInPictureActive =
      videoWindow.IsOpen() && videoWindow.IsPictureInPicture();
  overlayInputs.subtitleRenderError = videoWindow.GetSubtitleRenderError();
  overlayInputs.screenWidth = width;
  overlayInputs.screenHeight = height;
  overlayInputs.windowWidth = videoWindow.IsOpen() ? videoWindow.GetWidth() : 0;
  overlayInputs.windowHeight = videoWindow.IsOpen() ? videoWindow.GetHeight() : 0;
  overlayInputs.artTop = asciiArtTop;
  overlayInputs.progressBarX = frameOutput.progressBarX;
  overlayInputs.progressBarY = frameOutput.progressBarY;
  overlayInputs.progressBarWidth = frameOutput.progressBarWidth;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);
  const int hoverIndex =
      overlayControlHover.load(std::memory_order_relaxed);
  playback_overlay::OverlayCellLayout overlayLayout;
  int overlayReservedLines = overlayState.overlayVisible ? 5 : 0;
  if (overlayState.overlayVisible) {
    overlayLayout = playback_overlay::layoutPlaybackOverlayCells(
        overlayState, width, height, hoverIndex);
    if (overlayLayout.topY != -1) {
      const int overlayTop = std::max(0, overlayLayout.topY);
      overlayReservedLines = std::max(5, height - overlayTop);
    }
  }

  screen.clear(baseStyle);
  int headerY = 0;
  for (const auto& debugLine : debugLines) {
    screen.writeText(0, headerY++, fitLine(debugLine, width), dimStyle);
  }
  if (!statusLine.empty()) {
    screen.writeText(0, headerY++, fitLine(statusLine, width), dimStyle);
  }

  if (currentMode == PlaybackRenderMode::AsciiTerminal) {
    playback_frame_output::renderAsciiModeContent(
        screen, art, width, height, maxHeight, asciiArtTop, waitingLabel(),
        allowFrame, baseStyle, overlayVisibleNow, overlayReservedLines,
        dimStyle);
    playback_ascii_subtitles::RenderInput subtitleInput;
    subtitleInput.screen = &screen;
    subtitleInput.art = &art;
    subtitleInput.width = width;
    subtitleInput.height = height;
    subtitleInput.maxHeight = maxHeight;
    subtitleInput.artTop = asciiArtTop;
    subtitleInput.allowFrame = allowFrame;
    subtitleInput.overlayVisible = overlayVisibleNow;
    subtitleInput.overlayReservedLines = overlayReservedLines;
    subtitleInput.subtitleText = overlayState.subtitleText;
    subtitleInput.subtitleCues = &overlayState.subtitleCues;
    subtitleInput.assScript = overlayState.subtitleAssScript;
    subtitleInput.assFonts = overlayState.subtitleAssFonts;
    subtitleInput.subtitleClockUs = overlayState.subtitleClockUs;
    subtitleInput.baseStyle = baseStyle;
    subtitleInput.accentStyle = accentStyle;
    subtitleInput.dimStyle = dimStyle;
    playback_ascii_subtitles::renderAsciiSubtitles(subtitleInput);
  } else {
    playback_frame_output::renderNonAsciiModeContent(
        screen, windowActive, allowFrame, width, artTop, maxHeight, frame,
        videoWindow.GetWidth(), videoWindow.GetHeight(), dimStyle);
  }

  if (overlayState.overlayVisible) {
    for (const auto& item : overlayLayout.controls) {
      if (item.y < artTop || item.y < 0 || item.y >= height) continue;
      const int x = item.x;
      if (x >= width) break;
      const int avail = width - x;
      if (avail <= 0) break;
      std::string text = item.text;
      if (utf8DisplayWidth(text) > avail) {
        text = utf8TakeDisplayWidth(text, avail);
      }
      Style style = item.active ? accentStyle : baseStyle;
      if (item.hovered) {
        style = {style.bg, style.fg};
      }
      screen.writeText(x, item.y, text, style);
    }

    for (const auto& titleLine : overlayLayout.titleLines) {
      if (titleLine.y < artTop || titleLine.y < 0 ||
          titleLine.y >= height || titleLine.text.empty()) {
        continue;
      }
      screen.writeText(titleLine.x, titleLine.y, titleLine.text, accentStyle);
    }

    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }
    frameOutput.progressBarX = overlayLayout.progressBarX;
    frameOutput.progressBarY = overlayLayout.progressBarY;
    frameOutput.progressBarWidth = overlayLayout.progressBarWidth;
    if (frameOutput.progressBarY >= 0 && frameOutput.progressBarY < height &&
        frameOutput.progressBarWidth > 0) {
      const int leftFrameX = frameOutput.progressBarX - 1;
      const int rightFrameX =
          frameOutput.progressBarX + frameOutput.progressBarWidth;
      if (leftFrameX >= 0 && leftFrameX < width) {
        screen.writeChar(leftFrameX, frameOutput.progressBarY, L'|',
                         progressFrameStyle);
      }
      auto barCells = renderProgressBarCells(
          ratio, frameOutput.progressBarWidth, progressEmptyStyle, progressStart,
          progressEnd);
      for (int i = 0; i < frameOutput.progressBarWidth; ++i) {
        const int x = frameOutput.progressBarX + i;
        if (x < 0 || x >= width) continue;
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(x, frameOutput.progressBarY, cell.ch, cell.style);
      }
      if (rightFrameX >= 0 && rightFrameX < width) {
        screen.writeChar(rightFrameX, frameOutput.progressBarY, L'|',
                         progressFrameStyle);
      }
    }
    if (!overlayLayout.suffixText.empty() &&
        overlayLayout.suffixY >= artTop && overlayLayout.suffixY >= 0 &&
        overlayLayout.suffixY < height) {
      screen.writeText(overlayLayout.suffixX, overlayLayout.suffixY,
                       overlayLayout.suffixText, baseStyle);
    }
  }

  screen.draw();
}

}  // namespace playback_screen_renderer
