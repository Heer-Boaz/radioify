#include "screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "audioplayback.h"
#include "playback/debug/lines.h"
#include "playback/video/image.h"
#include "playback/video/state/machine.h"
#include "subtitles.h"
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
    case PlayerState::FrameStep:
      return "FrameStep";
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

bool updateTimelinePreviewArt(
    const playback_video_timeline_preview::Image& image, int width, int height,
    TimelinePreviewAsciiCache* cache) {
  if (!cache || width <= 0 || height <= 0) return false;
  if (cache->imageId == image.id && cache->width == width &&
      cache->height == height && cache->art.width == width &&
      cache->art.height == height) {
    return true;
  }

  const auto& surface = image.surface;
  cache->renderer.resetHistory();
  const bool rendered = playback_video_image::validate(surface) &&
                        cache->renderer.renderRgbaExact(
                            surface.pixels.data(), surface.width,
                            surface.height, width, height, cache->art, true);
  if (!rendered) return false;
  cache->imageId = image.id;
  cache->width = width;
  cache->height = height;
  return true;
}

void renderTimelinePreview(
    ConsoleScreen& screen,
    const playback_video_timeline_preview::Snapshot& snapshot,
    const playback_video_timeline_preview::CellLayout& layout,
    TimelinePreviewAsciiCache* cache,
    const playback_overlay::OverlayRenderStyles& styles) {
  if (!snapshot.hoverActive || !layout.drawable()) return;

  if (!snapshot.hasImage() ||
      !updateTimelinePreviewArt(*snapshot.image, layout.imageWidth,
                                layout.imageHeight, cache)) {
    playback_overlay::renderTimelinePreviewTimestampToScreen(screen, layout,
                                                              styles);
    return;
  }

  const AsciiArt& art = cache->art;
  for (int y = 0; y < layout.imageHeight; ++y) {
    for (int x = 0; x < layout.imageWidth; ++x) {
      const auto& cell = art.cells[static_cast<size_t>(y * art.width + x)];
      screen.writeChar(layout.imageX + x, layout.imageY + y, cell.ch,
                       Style{cell.fg,
                             cell.hasBg ? cell.bg : styles.baseStyle.bg});
    }
  }
  playback_overlay::renderTimelinePreviewChromeToScreen(
      screen, layout, styles);
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
  const bool overlayVisibleNow = inputs.osd.controlsVisible;
  bool clearHistory = inputs.clearHistory;
  bool frameChanged = inputs.frameChanged;
  bool frameAvailable = inputs.frameAvailable;
  double cellPixelWidth = inputs.cellPixelWidth;
  double cellPixelHeight = inputs.cellPixelHeight;
  const std::string& cellPixelSourceLabel = inputs.cellPixelSourceLabel;
  auto& enableSubtitlesShared = *inputs.enableSubtitlesShared;
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
    if (!enableAudio) {
      statusLine = "Audio disabled";
    } else if (player.audioTrackCount() > 0) {
      statusLine = "Audio unavailable";
    }
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
  if (debugOverlay) {
    debugLines.insert(debugLines.end(), inputs.debugLines.begin(),
                      inputs.debugLines.end());
  }
  if (debugOverlay && currentMode == PlaybackRenderMode::AsciiTerminal) {
    debugLines.push_back(
        playback_debug_lines::videoFrameDebugLine(player.debugInfo()));

    char buf[512];
    const char* cellSource =
        cellPixelSourceLabel.empty() ? "unknown" : cellPixelSourceLabel.c_str();
    std::snprintf(buf, sizeof(buf),
                  "DBG cell=%.2fx%.2f/%s cols=%d rows=%d",
                  cellPixelWidth, cellPixelHeight, cellSource, width, height);
    debugLines.emplace_back(buf);

    const int plannedStatusLines = statusLine.empty() ? 0 : 1;
    const int plannedMaxHeight = height - plannedStatusLines;
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
  int headerLines = statusLine.empty() ? 0 : 1;
  const int footerLines = 0;
  int artTop = headerLines;
  int maxHeight = height - headerLines - footerLines;
  int asciiArtTop = artTop;

  double currentSec = 0.0;
  double totalSec = -1.0;
  const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
  const int64_t clockUs = timeline.positionUs;
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
  const bool seekingOverlay = timeline.seekPending();
  const bool subtitlesEnabledNow =
      enableSubtitlesShared.load(std::memory_order_relaxed);
  const bool hasVideoStream =
      player.sourceWidth() > 0 && player.sourceHeight() > 0;
  bool waitingForAudio = audioOk && !audioStreamClockReady() && !audioIsFinished();
  bool audioStarved = audioOk && audioStreamStarved();
  bool waitingForVideo = hasVideoStream && !player.hasVideoFrame();
  const bool playerTransportPaused =
      playback_video_state_machine::project(player.state()).transport ==
      playback_video_state_machine::TransportState::Paused;
  bool isPaused =
      playbackState == PlaybackSessionState::Paused || playerTransportPaused;
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
  const bool pausedNow =
      playbackState == PlaybackSessionState::Paused || playerTransportPaused;
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
  overlayInputs.radioLabel = std::string(audioGetRadioFilterLabel());
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.canCycleAudioTracks = audioOk && player.canCycleAudioTracks();
  overlayInputs.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  overlayInputs.subtitleManager = &subtitleManager;
  overlayInputs.hasSubtitles = hasSubtitles;
  overlayInputs.subtitlesEnabled = subtitlesEnabledNow;
  overlayInputs.subtitleClockUs = timeline.sourcePositionUs;
  overlayInputs.seekingOverlay = seekingOverlay;
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  overlayInputs.osd = inputs.osd;
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
  overlayInputs.debugLines = debugLines;
  overlayInputs.videoEdit = inputs.videoEdit;
  overlayInputs.videoEditExport = inputs.videoEditExport;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);
  const int hoverIndex =
      overlayControlHover.load(std::memory_order_relaxed);
  playback_overlay::OverlayCellLayout overlayLayout;
  const bool showOverlay = overlayState.overlayVisible ||
                           !overlayState.debugLines.empty() ||
                            inputs.timelinePreview.hoverActive ||
                            overlayState.videoEdit.active ||
                            overlayState.videoEdit.exitConfirmation ||
                            overlayState.videoEditExport.running();
  int overlayReservedLines = showOverlay ? 5 : 0;
  if (showOverlay) {
    overlayLayout = playback_overlay::layoutPlaybackOverlayCells(
        overlayState, width, height, hoverIndex);
    if (overlayLayout.topY != -1) {
      const int overlayTop = std::max(0, overlayLayout.topY);
      overlayReservedLines = std::max(5, height - overlayTop);
    }
  }

  screen.clear(baseStyle);
  int headerY = 0;
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

  if (showOverlay) {
    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }
    frameOutput.progressBarX = overlayLayout.progressBarX;
    frameOutput.progressBarY = overlayLayout.progressBarY;
    frameOutput.progressBarWidth = overlayLayout.progressBarWidth;
    playback_overlay::OverlayRenderStyles overlayStyles{
        baseStyle, accentStyle, progressEmptyStyle, progressFrameStyle,
        progressStart, progressEnd};
    playback_overlay::renderOverlayToScreen(
        screen, overlayLayout, overlayStyles, ratio, &overlayState.videoEdit,
        &overlayState.videoEditExport, artTop, height);
  }

  if (inputs.timelinePreview.hoverActive) {
    const playback_video_image::RgbaImage* previewSurface =
        inputs.timelinePreview.hasImage()
            ? &inputs.timelinePreview.image->surface
            : nullptr;
    const int previewSourceWidth =
        previewSurface
            ? static_cast<int>(previewSurface->width)
            : std::max(16, inputs.timelinePreview.sourceWidth > 0
                               ? inputs.timelinePreview.sourceWidth
                               : layoutSourceW);
    const int previewSourceHeight =
        previewSurface
            ? static_cast<int>(previewSurface->height)
            : std::max(9, inputs.timelinePreview.sourceHeight > 0
                              ? inputs.timelinePreview.sourceHeight
                              : layoutSourceH);
    const auto previewLayout =
        playback_video_timeline_preview::layoutCells(
            width, height, overlayLayout.progressBarY,
            overlayLayout.progressBarX, overlayLayout.progressBarWidth,
            inputs.timelinePreview.anchorRatio, previewSourceWidth,
            previewSourceHeight, cellPixelWidth, cellPixelHeight,
            playback_video_timeline_preview::formatTimestamp(
                inputs.timelinePreview.targetUs));
    playback_overlay::OverlayRenderStyles previewStyles;
    previewStyles.baseStyle = baseStyle;
    previewStyles.accentStyle = accentStyle;
    renderTimelinePreview(screen, inputs.timelinePreview, previewLayout,
                          inputs.timelinePreviewCache, previewStyles);
  }

  if (overlayState.transientMessage) {
    playback_overlay::renderTransientMessageToScreen(
        screen, *overlayState.transientMessage, accentStyle);
  }

  screen.draw();
}

}  // namespace playback_screen_renderer
