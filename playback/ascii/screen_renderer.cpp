#include "screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "playback/debug/lines.h"
#include "playback/video/gpu/gpu_runtime.h"
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

  if (!snapshot.metadataLines.empty()) {
    for (int y = layout.outerY;
         y < layout.outerY + layout.outerHeight; ++y) {
      for (int x = layout.outerX;
           x < layout.outerX + layout.outerWidth; ++x) {
        screen.writeChar(x, y, L' ', styles.baseStyle);
      }
    }
  }

  if (!snapshot.hasImage() ||
      !updateTimelinePreviewArt(*snapshot.image, layout.imageWidth,
                                layout.imageHeight, cache)) {
    if (snapshot.metadataLines.empty()) {
      playback_overlay::renderTimelinePreviewTimestampToScreen(screen, layout,
                                                                styles);
    } else {
      playback_overlay::renderTimelinePreviewChromeToScreen(screen, layout,
                                                             styles);
    }
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

void renderPlaybackScreen(const PlaybackScreenResources& resources,
                          PlaybackScreenTarget& target,
                          const PlaybackScreenModel& model) {
  auto& screen = target.screen;
  auto& gpuRenderer = resources.gpu.asciiRenderer();
  auto& frameCache = target.frameCache;
  auto& art = target.art;
  VideoFrame* frame = &target.frame;
  const Style& baseStyle = resources.baseStyle;
  const Style& accentStyle = resources.accentStyle;
  const Style& dimStyle = resources.dimStyle;
  const Style& progressEmptyStyle = resources.progressEmptyStyle;
  const Style& progressFrameStyle = resources.progressFrameStyle;
  const Color& progressStart = resources.progressStart;
  const Color& progressEnd = resources.progressEnd;
  const bool debugOverlay = model.debugOverlay;
  const PlaybackVisualMode visualMode = model.visualMode;
  const PlaybackSessionState playbackState = model.playbackState;
  const bool enableAudio = model.enableAudio;
  const bool audioOk = model.audioOk;
  const bool audioStarting = model.audioStarting;
  const bool nativeWindowActive = model.nativeWindowActive;
  const bool allowAsciiCpuFallback = model.allowAsciiCpuFallback;
  const bool overlayVisibleNow = model.overlay.overlayVisible;
  const bool clearHistory = model.clearHistory;
  const bool frameChanged = model.frameChanged;
  const bool frameAvailable = model.frameAvailable;
  const double cellPixelWidth = model.cellPixelWidth;
  const double cellPixelHeight = model.cellPixelHeight;
  const std::string& cellPixelSourceLabel = model.cellPixelSourceLabel;
  const PlaybackMediaPresentation& media = model.media;
  const PlaybackAudioPresentation& audio = media.audio;
  playback_frame_output::FrameOutputState& frameOutput = target.frameOutput;
  const auto& warningSink = resources.warningSink;
  const auto& timingSink = resources.timingSink;
  screen.updateSize();
  int width = screen.width();
  int height = screen.height();
  std::string statusLine;
  if (!audioOk && !audioStarting) {
    if (!enableAudio) {
      statusLine = "Audio disabled";
    } else if (media.audioTrackCount > 0) {
      statusLine = "Audio unavailable";
    }
  }
  auto [frameDisplayW, frameDisplayH] = frameDisplaySize(frame);
  int layoutSourceW = media.sourceWidth;
  int layoutSourceH = media.sourceHeight;
  const char* layoutSourceKind = "player";
  if (layoutSourceW <= 0 || layoutSourceH <= 0) {
    layoutSourceW = frameDisplayW;
    layoutSourceH = frameDisplayH;
    layoutSourceKind = "frame";
  }
  std::vector<std::string> debugLines;
  if (debugOverlay) {
    debugLines.insert(debugLines.end(), model.overlay.debugLines.begin(),
                      model.overlay.debugLines.end());
  }
  if (debugOverlay && visualMode == PlaybackVisualMode::AsciiGrid) {
    debugLines.push_back(
        playback_debug_lines::videoFrameDebugLine(media.debug));

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
    std::snprintf(buf2, sizeof(buf2),
                  "DBG ascii src=%dx%d(%s) frame=%dx%d r=%d art=%dx%d "
                  "phys=%.0fx%.0f asp=%.3f srcasp=%.3f path=%s",
                  layoutSourceW, layoutSourceH, layoutSourceKind, frameDisplayW,
                  frameDisplayH, frame ? frame->rotationQuarterTurns : 0,
                  plannedArtW, plannedArtH, physW, physH, physAspect,
                  sourceAspect,
                  frameOutput.lastRenderPath.empty()
                      ? "none"
                      : frameOutput.lastRenderPath.c_str());
    debugLines.emplace_back(buf2);
  }
#if RADIOIFY_ENABLE_TIMING_LOG
  if (debugOverlay) {
    const PlayerDebugInfo& dbg = media.debug;
    char buf1[256];
    char buf2[256];
    double masterSec = static_cast<double>(dbg.masterClockUs) / 1000000.0;
    double diffMs = static_cast<double>(dbg.lastDiffUs) / 1000.0;
    double delayMs = static_cast<double>(dbg.lastDelayUs) / 1000.0;
    double durationMs =
        static_cast<double>(dbg.lastPresentedDurationUs) / 1000.0;
    std::snprintf(
        buf1, sizeof(buf1),
        "DBG state=%s serial=%d seek=%d qv=%zu dur=%.1fms master=%s %.3fs "
        "diff=%.1fms delay=%.1fms",
        playerStateLabel(dbg.state), dbg.currentSerial, dbg.pendingSeekSerial,
        dbg.videoQueueDepth, durationMs, clockSourceLabel(dbg.masterSource),
        masterSec, diffMs, delayMs);
    std::snprintf(buf2, sizeof(buf2),
                  "DBG audio ok=%d ready=%d fresh=%d starved=%d buf=%zuf "
                  "rate=%u clock=%.3fs",
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
  const PlayerTimelineSnapshot& timeline = media.timeline;
  const int64_t clockUs = timeline.positionUs;
  if (clockUs > 0) {
    currentSec = static_cast<double>(clockUs) / 1000000.0;
  }
  const int64_t durUs = media.durationUs;
  if (durUs > 0) {
    totalSec = static_cast<double>(durUs) / 1000000.0;
  } else if (audioOk) {
    totalSec = audio.durationSec;
  }
  if (totalSec > 0.0) {
    currentSec = std::clamp(currentSec, 0.0, totalSec);
  }
  double displaySec = currentSec;
  const bool seekingOverlay = timeline.seekPending();
  const bool hasVideoStream =
      media.sourceWidth > 0 && media.sourceHeight > 0;
  const bool waitingForAudio =
      audioOk && !audio.streamClockReady && !audio.finished;
  const bool audioStarved = audioOk && audio.streamStarved;
  const bool waitingForVideo = hasVideoStream && !media.debug.hasVideoFrame;
  const bool playerTransportPaused =
      playback_video_state_machine::project(media.debug.state).transport ==
      playback_video_state_machine::TransportState::Paused;
  bool isPaused =
      playbackState == PlaybackSessionState::Paused || playerTransportPaused;
  frameOutput.haveFrame = frameAvailable;
  bool allowFrame = frameOutput.haveFrame && !nativeWindowActive;

  auto waitingLabel = [&]() -> std::string {
    if (playbackState == PlaybackSessionState::Ended) return "Ended";
    if (seekingOverlay) return "Seeking...";
    if (isPaused) return "Paused";
    if (media.debug.state == PlayerState::Opening) return "Opening...";
    if (media.debug.state == PlayerState::Prefill) return "Prefilling...";
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

  if (visualMode == PlaybackVisualMode::AsciiGrid) {
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
    playback_frame_output::prepareAsciiModeFrame(resources.gpu, asciiInput);
  } else {
    playback_frame_output::prepareNonAsciiModeFrame(
        allowFrame, width, maxHeight, frame->width, frame->height,
        frameOutput, warningSink);
  }

  if (visualMode == PlaybackVisualMode::AsciiGrid && allowFrame &&
      art.width > 0 && art.height > 0) {
    const int visibleArtHeight = std::min(art.height, maxHeight);
    asciiArtTop = playback_frame_output::centerContentTop(
        artTop, maxHeight, visibleArtHeight);
  }

  frameOutput.overlayInteractions = {};
  playback_overlay::PlaybackOverlayState overlayState = model.overlay;
  overlayState.debugLines = std::move(debugLines);
  overlayState.chromeVisible =
      overlayState.chromeVisible || !overlayState.debugLines.empty();
  const int hoverIndex = model.controlHoverToken;
  playback_overlay::OverlayCellLayout overlayLayout;
  playback_overlay::ContextMenuCellLayout contextMenuLayout;
  const bool showPlaybackChrome =
      overlayState.chromeVisible || model.timelinePreview.hoverActive;
  const bool showContextMenu = overlayState.contextMenu.visible;
  int overlayReservedLines = showPlaybackChrome ? 5 : 0;
  if (showPlaybackChrome || showContextMenu) {
    overlayLayout = playback_overlay::layoutPlaybackOverlayCells(
        overlayState, width, height, hoverIndex);
    if (showPlaybackChrome && overlayLayout.topY != -1) {
      const int overlayTop = std::max(0, overlayLayout.topY);
      overlayReservedLines = std::max(5, height - overlayTop);
    }
    contextMenuLayout = playback_overlay::layoutContextMenuCells(
        overlayState.contextMenu, width, height);
  }

  screen.clear(baseStyle);
  int headerY = 0;
  if (!statusLine.empty()) {
    screen.writeText(0, headerY++, fitLine(statusLine, width), dimStyle);
  }

  if (visualMode == PlaybackVisualMode::AsciiGrid) {
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
        screen, nativeWindowActive, allowFrame, width, artTop, maxHeight, frame,
        model.nativeWindowWidth, model.nativeWindowHeight, dimStyle);
  }

  if (showPlaybackChrome || showContextMenu) {
    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }
    if (showPlaybackChrome) {
      frameOutput.overlayInteractions =
          playback_overlay::buildOverlayInteractionMap(
              overlayLayout, &overlayState.videoEdit,
              overlayState.videoEditPrompt,
              overlayState.mediaActionConfirmationPrompt.has_value());
    }
    if (showContextMenu && contextMenuLayout.drawable()) {
      frameOutput.overlayInteractions =
          playback_overlay::buildContextMenuInteractionMap(contextMenuLayout);
    }
    playback_overlay::OverlayRenderStyles overlayStyles;
    overlayStyles.baseStyle = baseStyle;
    overlayStyles.accentStyle = accentStyle;
    overlayStyles.progressEmptyStyle = progressEmptyStyle;
    overlayStyles.progressFrameStyle = progressFrameStyle;
    overlayStyles.progressStart = progressStart;
    overlayStyles.progressEnd = progressEnd;
    if (showPlaybackChrome) {
      playback_overlay::renderOverlayToScreen(
          screen, overlayLayout, overlayStyles, ratio, &overlayState.videoEdit,
          &overlayState.videoEditExport,
          overlayState.videoEditPrompt,
          overlayState.mediaActionConfirmationPrompt, &overlayState.chapters,
          overlayState.chapterOverviewOpen, artTop, height);
    }
  }

  if (model.timelinePreview.hoverActive) {
    const playback_video_image::RgbaImage* previewSurface =
        model.timelinePreview.hasImage()
            ? &model.timelinePreview.image->surface
            : nullptr;
    const int previewSourceWidth =
        previewSurface
            ? static_cast<int>(previewSurface->width)
            : std::max(16, model.timelinePreview.sourceWidth > 0
                               ? model.timelinePreview.sourceWidth
                               : layoutSourceW);
    const int previewSourceHeight =
        previewSurface
            ? static_cast<int>(previewSurface->height)
            : std::max(9, model.timelinePreview.sourceHeight > 0
                              ? model.timelinePreview.sourceHeight
                              : layoutSourceH);
    const auto previewLayout =
        playback_video_timeline_preview::layoutCells(
            width, height, overlayLayout.progressBarY,
            overlayLayout.progressBarX, overlayLayout.progressBarWidth,
            model.timelinePreview.anchorRatio, previewSourceWidth,
            previewSourceHeight, cellPixelWidth, cellPixelHeight,
            playback_video_timeline_preview::formatTimestamp(
                model.timelinePreview.targetUs),
            model.timelinePreview.metadataLines);
    playback_overlay::OverlayRenderStyles previewStyles;
    previewStyles.baseStyle = baseStyle;
    previewStyles.accentStyle = accentStyle;
    renderTimelinePreview(screen, model.timelinePreview, previewLayout,
                          &target.timelinePreviewCache, previewStyles);
  }

  if (overlayState.transientMessage) {
    playback_overlay::renderTransientMessageToScreen(
        screen, *overlayState.transientMessage, accentStyle);
  }

  if (contextMenuLayout.drawable()) {
    playback_overlay::OverlayRenderStyles menuStyles;
    menuStyles.baseStyle = baseStyle;
    menuStyles.accentStyle = accentStyle;
    menuStyles.progressEmptyStyle = progressEmptyStyle;
    menuStyles.progressFrameStyle = progressFrameStyle;
    menuStyles.progressStart = progressStart;
    menuStyles.progressEnd = progressEnd;
    playback_overlay::renderContextMenuToScreen(screen, contextMenuLayout,
                                                menuStyles);
  }

  screen.draw();
}

}  // namespace playback_screen_renderer
