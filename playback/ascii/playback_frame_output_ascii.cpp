#include "playback_frame_output.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "asciiart_layout.h"
#include "video_frame_cache_update.h"

namespace {
void emitWarning(
    const playback_frame_output::LogLineWriter& sink,
    const std::string& message) {
  if (sink) {
    sink(message);
  }
}

void setFailure(playback_frame_output::AsciiModePrepareInput& input,
                const std::string& message,
                const std::string& detail) {
  if (!input.state) return;
  input.state->renderFailed = true;
  input.state->renderFailMessage = message;
  input.state->renderFailDetail = detail;
  input.state->haveFrame = false;
}

bool logAsciiRendererStartup(const VideoFrame& frame,
                            const AsciiArt& art,
                            const std::function<void(const std::string&)>&
                                timingSink) {
  static bool logged = false;
  if (logged) {
    return false;
  }
  logged = true;
  if (!timingSink) return false;

  const char* formatName = "";
  switch (frame.format) {
    case VideoPixelFormat::HWTexture:
      formatName = "hwtexture";
      break;
    case VideoPixelFormat::NV12:
      formatName = "nv12";
      break;
    case VideoPixelFormat::P010:
      formatName = "p010";
      break;
    case VideoPixelFormat::RGB32:
      formatName = "rgba";
      break;
    case VideoPixelFormat::ARGB32:
      formatName = "rgba";
      break;
    default:
      formatName = "unknown";
      break;
  }
  char inputBuf[256];
  std::snprintf(inputBuf, sizeof(inputBuf),
                "video_renderer_input format=%s in=%dx%d out=%dx%d",
                formatName, frame.width, frame.height, art.width, art.height);
  timingSink(std::string(inputBuf));
  return true;
}

bool cpuRenderFallback(const playback_frame_output::AsciiModePrepareInput& input) {
  if (input.frame->format == VideoPixelFormat::NV12 ||
      input.frame->format == VideoPixelFormat::P010) {
    bool is10Bit = input.frame->format == VideoPixelFormat::P010;
    return renderAsciiArtFromYuvExact(
        input.frame->yuv.data(), input.frame->width, input.frame->height,
        input.frame->stride, input.frame->planeHeight,
        is10Bit ? YuvFormat::P010 : YuvFormat::NV12, input.frame->fullRange,
        input.frame->yuvMatrix, input.frame->yuvTransfer, input.art->width,
        input.art->height, *input.art);
  }

  if (input.frame->format == VideoPixelFormat::RGB32 ||
      input.frame->format == VideoPixelFormat::ARGB32) {
    return renderAsciiArtFromRgbaExact(
        input.frame->rgba.data(), input.frame->width, input.frame->height,
        input.art->width, input.art->height, *input.art,
        input.frame->format == VideoPixelFormat::ARGB32);
  }

  return false;
}

std::pair<int, int> sourceAspectTargetSize(int targetPixelW, int targetPixelH,
                                          int srcW, int srcH) {
  if (srcW <= 0 || srcH <= 0) {
    return {targetPixelW, targetPixelH};
  }

  const double aspect =
      static_cast<double>(srcW) / static_cast<double>(srcH);
  int targetW = targetPixelW;
  int targetH = static_cast<int>(
      std::lround(static_cast<double>(targetW) / aspect));
  if (targetH < targetPixelH) {
    targetH = targetPixelH;
    targetW = static_cast<int>(
        std::lround(static_cast<double>(targetH) * aspect));
  }
  return {targetW, targetH};
}

std::pair<int, int> computeFittedAsciiOutputSize(int maxWidth,
                                                 int maxHeight,
                                                 int srcW,
                                                 int srcH,
                                                 double cellPixelWidth,
                                                 double cellPixelHeight) {
  AsciiArtLayout fitted = fitAsciiArtLayout(
      srcW, srcH, maxWidth, maxHeight, cellPixelWidth, cellPixelHeight);
  return {fitted.width, fitted.height};
}

}  // namespace

namespace playback_frame_output {

std::pair<int, int> computeAsciiPlaybackTargetSize(int width, int height,
                                                  int srcW, int srcH,
                                                  double cellPixelWidth,
                                                  double cellPixelHeight,
                                                  bool showStatusLine) {
  int headerLines = showStatusLine ? 1 : 0;
  const int footerLines = 0;
  int maxHeight = height - headerLines - footerLines;
  auto [outW, outH] = computeFittedAsciiOutputSize(
      width, maxHeight, srcW, srcH, cellPixelWidth,
      cellPixelHeight);

  const int targetPixelW =
      static_cast<int>(std::lround(outW * cellPixelWidth));
  const int targetPixelH =
      static_cast<int>(std::lround(outH * cellPixelHeight));
  auto [targetW, targetH] =
      sourceAspectTargetSize(targetPixelW, targetPixelH, srcW, srcH);
  return std::pair<int, int>(targetW, targetH);
}

std::pair<int, int> computeAsciiOutputSize(int maxWidth, int maxHeight,
                                          int srcW, int srcH,
                                          double cellPixelWidth,
                                          double cellPixelHeight) {
  return computeFittedAsciiOutputSize(maxWidth, maxHeight, srcW, srcH,
                                      cellPixelWidth, cellPixelHeight);
}

bool prepareAsciiModeFrame(AsciiModePrepareInput& input) {
  if (!input.state) {
    return false;
  }
  FrameOutputState& state = *input.state;
  if (!input.allowFrame) {
    state.lastRenderPath.clear();
    state.cachedWidth = -1;
    state.cachedMaxHeight = -1;
    state.cachedFrameWidth = -1;
    state.cachedFrameHeight = -1;
    state.cachedLayoutSourceWidth = -1;
    state.cachedLayoutSourceHeight = -1;
    state.cachedCellPixelWidth = -1.0;
    state.cachedCellPixelHeight = -1.0;
    return false;
  }
  if (!(input.frameChanged || input.clearHistory || input.sizeChanged)) {
    return false;
  }
  if (!input.frame || !input.art || !input.gpuRenderer || !input.frameCache) {
    setFailure(input, "Invalid frame rendering context.",
               "Missing frame, art, renderer, or frame cache.");
    return false;
  }
  if (input.frame->width <= 0 || input.frame->height <= 0) {
    emitWarning(input.warningSink, "Skipping video frame with invalid dimensions.");
    state.haveFrame = false;
    return false;
  }

  int layoutSrcW = input.sourceWidth;
  int layoutSrcH = input.sourceHeight;
  if (layoutSrcW <= 0 || layoutSrcH <= 0) {
    layoutSrcW = ((input.frame->rotationQuarterTurns & 1) != 0)
                     ? input.frame->height
                     : input.frame->width;
    layoutSrcH = ((input.frame->rotationQuarterTurns & 1) != 0)
                     ? input.frame->width
                     : input.frame->height;
  }
  auto [outW, outH] = input.computeAsciiOutputSize(
      input.width, input.maxHeight, layoutSrcW, layoutSrcH,
      input.cellPixelWidth, input.cellPixelHeight);
  const int prevArtWidth = input.art->width;
  const int prevArtHeight = input.art->height;
  const size_t prevArtCellCount =
      static_cast<size_t>(std::max(0, prevArtWidth)) *
      static_cast<size_t>(std::max(0, prevArtHeight));
  const bool hadRenderedArt =
      prevArtWidth > 0 && prevArtHeight > 0 &&
      input.art->cells.size() >= prevArtCellCount;
  input.art->width = outW;
  input.art->height = outH;

  bool cacheUpdated = false;
  bool renderFromCache = false;
  bool keepPreviousFrame = false;
  bool hadCachedFrame = false;
  std::string gpuErr;

  try {
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
      setFailure(input, "GPU device unavailable.",
                 "Shared GPU device was not initialized.");
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) {
      setFailure(input, "GPU context unavailable.",
                 "Failed to acquire D3D11 immediate context.");
      return false;
    }

    const auto start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
      hadCachedFrame = input.frameCache->HasFrame();
      if (input.clearHistory) {
        input.gpuRenderer->ClearHistory();
      }
      cacheUpdated = playback_video_frame_cache::update(
          *input.frameCache, device, context.Get(), *input.frame);
      if (cacheUpdated || hadCachedFrame) {
        renderFromCache = input.gpuRenderer->RenderFromCache(*input.frameCache,
                                                            *input.art, &gpuErr);
        if (!renderFromCache && hadRenderedArt) {
          keepPreviousFrame = true;
        }
      } else if (hadRenderedArt) {
        keepPreviousFrame = true;
      }
    }
    const auto end = std::chrono::steady_clock::now();

    const auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           end - start)
                           .count();
    if (durMs > 50 && input.timingSink) {
      char buf[256];
      std::snprintf(
          buf, sizeof(buf), "video_render_slow pts_us=%lld dur_ms=%lld",
          static_cast<long long>(input.frame->timestamp100ns / 10),
          static_cast<long long>(durMs));
      input.timingSink(std::string(buf));
    }

    bool asciiOk = false;
    if (renderFromCache) {
      asciiOk = true;
      state.lastRenderPath = "gpu";
      if (input.timingSink && logAsciiRendererStartup(*input.frame, *input.art,
                                                     input.timingSink)) {
        char buf[256];
        std::string details = input.gpuRenderer->lastNv12TextureDetail();
        if (details.empty()) {
          details = std::string("path=") + input.gpuRenderer->lastNv12TexturePath();
        }
        const char* fmt = "unknown";
        switch (input.frame->format) {
          case VideoPixelFormat::HWTexture:
            fmt = "hwtexture";
            break;
          case VideoPixelFormat::NV12:
            fmt = "nv12";
            break;
          case VideoPixelFormat::P010:
            fmt = "p010";
            break;
          case VideoPixelFormat::RGB32:
          case VideoPixelFormat::ARGB32:
            fmt = "rgba";
            break;
          default:
            fmt = "unsupported";
            break;
        }
        std::snprintf(buf, sizeof(buf),
                      "video_renderer gpu_active=1 format=%s %s", fmt,
                      details.c_str());
        input.timingSink(std::string(buf));
      }
    } else if (keepPreviousFrame) {
      input.art->width = prevArtWidth;
      input.art->height = prevArtHeight;
      asciiOk = true;
      state.lastRenderPath = "previous-frame";
    } else if (cacheUpdated && gpuErr.empty()) {
      input.art->width = prevArtWidth;
      input.art->height = prevArtHeight;
      return false;
    } else if (input.allowAsciiCpuFallback ||
               input.frame->format == VideoPixelFormat::NV12 ||
               input.frame->format == VideoPixelFormat::P010 ||
               input.frame->format == VideoPixelFormat::RGB32 ||
               input.frame->format == VideoPixelFormat::ARGB32) {
      if (!cacheUpdated) {
        emitWarning(input.warningSink,
                    "GPU cache update failed; falling back to CPU ASCII path.");
      } else if (!gpuErr.empty()) {
        emitWarning(
            input.warningSink,
            std::string("GPU ASCII render failed; falling back to CPU path: ") +
                gpuErr);
      } else {
        emitWarning(input.warningSink,
                    "GPU ASCII render failed; falling back to CPU path.");
      }
      asciiOk = cpuRenderFallback(input);
      if (asciiOk) {
        state.lastRenderPath = "cpu-fallback";
      }
    } else {
      emitWarning(input.warningSink,
                  gpuErr.empty() ? "GPU renderer temporarily unavailable."
                                 : gpuErr);
      state.haveFrame = false;
      return false;
    }

    if (!asciiOk) {
      setFailure(input, "Failed to render video frame.", "");
      return false;
    }
  } catch (...) {
    setFailure(input, "Failed to render video frame.", "");
    return false;
  }

  state.cachedWidth = input.width;
  state.cachedMaxHeight = input.maxHeight;
  state.cachedFrameWidth = input.frame->width;
  state.cachedFrameHeight = input.frame->height;
  state.cachedLayoutSourceWidth = layoutSrcW;
  state.cachedLayoutSourceHeight = layoutSrcH;
  state.cachedCellPixelWidth = input.cellPixelWidth;
  state.cachedCellPixelHeight = input.cellPixelHeight;
  return true;
}

void renderAsciiModeContent(ConsoleScreen& screen, const AsciiArt& art, int width,
                           int height, int maxHeight, int artTop,
                           const std::string& waitingLabel, bool allowFrame,
                           const Style& baseStyle, bool overlayVisible,
                           int overlayReservedLines,
                           const Style& dimStyle) {
  (void)overlayVisible;
  (void)overlayReservedLines;
  const int visibleArtWidth = std::min(art.width, width);
  const int visibleArtHeightLimit = std::min(art.height, maxHeight);
  const int availableHeight = height - artTop;
  const int visibleArtHeight = std::min(visibleArtHeightLimit, availableHeight);
  const int sourceX = std::max(0, (art.width - visibleArtWidth) / 2);
  const int sourceY = std::max(0, (art.height - visibleArtHeight) / 2);
  const int artX = std::max(0, (width - visibleArtWidth) / 2);
  const size_t expectedCellCount = static_cast<size_t>(std::max(0, art.width)) *
                                   static_cast<size_t>(std::max(0, art.height));
  const bool canDrawArt =
      allowFrame && visibleArtWidth > 0 && visibleArtHeightLimit > 0 &&
      art.cells.size() >= expectedCellCount;

  if (canDrawArt && visibleArtHeight > 0) {
    for (int y = 0; y < visibleArtHeight; ++y) {
      const int sourceRow = sourceY + y;
      for (int x = 0; x < visibleArtWidth; ++x) {
        const int sourceCol = sourceX + x;
        const auto& cell =
            art.cells[static_cast<size_t>(sourceRow * art.width + sourceCol)];
        Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
        screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
      }
    }
  } else if (!allowFrame) {
    screen.writeText(0, artTop, fitLine(waitingLabel, width), dimStyle);
  }
}

}  // namespace playback_frame_output
