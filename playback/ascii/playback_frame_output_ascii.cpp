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
  if (input.renderFailed) {
    *input.renderFailed = true;
  }
  if (input.renderFailMessage) {
    *input.renderFailMessage = message;
  }
  if (input.renderFailDetail) {
    *input.renderFailDetail = detail;
  }
  if (input.haveFrame) {
    *input.haveFrame = false;
  }
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

bool updateFrameCache(GpuVideoFrameCache& cache, ID3D11Device* device,
                     ID3D11DeviceContext* context, const VideoFrame& frame) {
  if (frame.format == VideoPixelFormat::HWTexture) {
    if (!frame.hwTexture) {
      return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.hwTexture->GetDesc(&desc);
    const bool is10Bit = (desc.Format == DXGI_FORMAT_P010);
    return cache.Update(device, context, frame.hwTexture.Get(),
                        frame.hwTextureArrayIndex, frame.width, frame.height,
                        frame.fullRange, frame.yuvMatrix, frame.yuvTransfer,
                        is10Bit ? 10 : 8, frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::NV12 ||
      frame.format == VideoPixelFormat::P010) {
    if (frame.stride <= 0 || frame.planeHeight <= 0 || frame.yuv.empty()) {
      return false;
    }
    const size_t strideBytes = static_cast<size_t>(frame.stride);
    const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
    const size_t yBytes = strideBytes * planeHeight;
    if (planeHeight == 0 || strideBytes == 0 ||
        yBytes / strideBytes != planeHeight) {
      return false;
    }
    return cache.UpdateNV12(
        device, context, frame.yuv.data(), frame.stride, frame.planeHeight,
        frame.width, frame.height, frame.fullRange, frame.yuvMatrix,
        frame.yuvTransfer, frame.format == VideoPixelFormat::P010 ? 10 : 8,
        frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    if (frame.rgba.empty()) {
      return false;
    }
    const int stride = frame.stride > 0 ? frame.stride : frame.width * 4;
    return cache.Update(device, context, frame.rgba.data(), stride, frame.width,
                        frame.height, frame.rotationQuarterTurns);
  }

  return false;
}

bool cpuRenderFallback(const playback_frame_output::AsciiModePrepareInput& input) {
  if (input.frame->format == VideoPixelFormat::NV12 ||
      input.frame->format == VideoPixelFormat::P010) {
    bool is10Bit = input.frame->format == VideoPixelFormat::P010;
    return renderAsciiArtFromYuv(
        input.frame->yuv.data(), input.frame->width, input.frame->height,
        input.frame->stride, input.frame->planeHeight,
        is10Bit ? YuvFormat::P010 : YuvFormat::NV12, input.frame->fullRange,
        input.frame->yuvMatrix, input.frame->yuvTransfer, input.art->width,
        input.art->height, *input.art);
  }

  if (input.frame->format == VideoPixelFormat::RGB32 ||
      input.frame->format == VideoPixelFormat::ARGB32) {
    return renderAsciiArtFromRgba(input.frame->rgba.data(), input.frame->width,
                                  input.frame->height, input.art->width,
                                  input.art->height, *input.art,
                                  input.frame->format == VideoPixelFormat::ARGB32);
  }

  return false;
}

int evenAtLeast(int value, int minimum) {
  value = std::max(value, minimum);
  if (value & 1) --value;
  return std::max(minimum, value);
}

std::pair<int, int> sourceAspectTargetSize(int minTargetW, int minTargetH,
                                          int srcW, int srcH) {
  minTargetW = evenAtLeast(minTargetW, 2);
  minTargetH = evenAtLeast(minTargetH, 4);
  if (srcW <= 0 || srcH <= 0) {
    return {minTargetW, minTargetH};
  }

  const double aspect =
      static_cast<double>(srcW) / static_cast<double>(srcH);
  int targetW = minTargetW;
  int targetH = evenAtLeast(
      static_cast<int>(std::lround(static_cast<double>(targetW) / aspect)), 4);
  if (targetH < minTargetH) {
    targetH = minTargetH;
    targetW = evenAtLeast(static_cast<int>(
                              std::lround(static_cast<double>(targetH) *
                                          aspect)),
                          2);
  }
  return {targetW, targetH};
}

void clampTargetSizeToSourceAndBudget(int& targetW, int& targetH, int srcW,
                                      int srcH) {
  const int kMaxDecodeWidth = 1024;
  const int kMaxDecodeHeight = 768;
  int maxW = kMaxDecodeWidth;
  int maxH = kMaxDecodeHeight;
  if (srcW > 1) maxW = std::min(maxW, srcW & ~1);
  if (srcH > 1) maxH = std::min(maxH, srcH & ~1);

  if (targetW <= maxW && targetH <= maxH) return;

  const double scaleW = static_cast<double>(maxW) / targetW;
  const double scaleH = static_cast<double>(maxH) / targetH;
  const double scale = std::min(scaleW, scaleH);
  targetW = evenAtLeast(static_cast<int>(
                            std::lround(static_cast<double>(targetW) * scale)),
                        2);
  targetH = evenAtLeast(static_cast<int>(
                            std::lround(static_cast<double>(targetH) * scale)),
                        4);
}

std::pair<int, int> computeCoveringAsciiOutputSize(int maxWidth,
                                                   int maxHeight,
                                                   int srcW,
                                                   int srcH,
                                                   int cellPixelWidth,
                                                   int cellPixelHeight) {
  maxWidth = std::max(1, maxWidth);
  maxHeight = std::max(1, maxHeight);
  cellPixelWidth = std::max(1, cellPixelWidth);
  cellPixelHeight = std::max(1, cellPixelHeight);

  AsciiArtLayout fitted =
      fitAsciiArtLayout(srcW, srcH, maxWidth, maxHeight, cellPixelWidth,
                        cellPixelHeight);
  if (srcW <= 0 || srcH <= 0) {
    return {fitted.width, fitted.height};
  }

  const double scaleW =
      (static_cast<double>(cellPixelWidth) * maxWidth) / srcW;
  const double scaleH =
      (static_cast<double>(cellPixelHeight) * maxHeight) / srcH;
  const double scale = std::max(scaleW, scaleH);

  int outW = static_cast<int>(
      std::lround(static_cast<double>(srcW) * scale / cellPixelWidth));
  int outH = static_cast<int>(
      std::lround(static_cast<double>(srcH) * scale / cellPixelHeight));
  return {std::max(1, outW), std::max(1, outH)};
}

}  // namespace

namespace playback_frame_output {

std::pair<int, int> computeAsciiPlaybackTargetSize(int width, int height,
                                                  int srcW, int srcH,
                                                  int cellPixelWidth,
                                                  int cellPixelHeight,
                                                  bool showStatusLine) {
  int headerLines = showStatusLine ? 1 : 0;
  const int footerLines = 0;
  int maxHeight = std::max(1, height - headerLines - footerLines);
  int maxOutW = std::max(1, width);
  auto [outW, outH] = computeCoveringAsciiOutputSize(
      maxOutW, std::max(1, maxHeight), srcW, srcH, cellPixelWidth,
      cellPixelHeight);

  auto [targetW, targetH] =
      sourceAspectTargetSize(std::max(2, outW * 2), std::max(4, outH * 4),
                             srcW, srcH);
  clampTargetSizeToSourceAndBudget(targetW, targetH, srcW, srcH);
  return std::pair<int, int>(targetW, targetH);
}

std::pair<int, int> computeAsciiOutputSize(int maxWidth, int maxHeight,
                                          int srcW, int srcH,
                                          int cellPixelWidth,
                                          int cellPixelHeight) {
  return computeCoveringAsciiOutputSize(maxWidth, maxHeight, srcW, srcH,
                                        cellPixelWidth, cellPixelHeight);
}

bool prepareAsciiModeFrame(AsciiModePrepareInput& input) {
  if (!input.allowFrame) {
    if (input.cachedWidth) *input.cachedWidth = -1;
    if (input.cachedMaxHeight) *input.cachedMaxHeight = -1;
    if (input.cachedFrameWidth) *input.cachedFrameWidth = -1;
    if (input.cachedFrameHeight) *input.cachedFrameHeight = -1;
    if (input.cachedCellPixelWidth) *input.cachedCellPixelWidth = -1;
    if (input.cachedCellPixelHeight) *input.cachedCellPixelHeight = -1;
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
    if (input.haveFrame) {
      *input.haveFrame = false;
    }
    return false;
  }

  auto [outW, outH] = input.computeAsciiOutputSize(
      input.width, input.maxHeight,
      ((input.frame->rotationQuarterTurns & 1) != 0) ? input.frame->height
                                                    : input.frame->width,
      ((input.frame->rotationQuarterTurns & 1) != 0) ? input.frame->width
                                                    : input.frame->height,
      input.cellPixelWidth, input.cellPixelHeight);
  const int prevArtWidth = input.art->width;
  const int prevArtHeight = input.art->height;
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
      cacheUpdated = updateFrameCache(*input.frameCache, device, context.Get(),
                                     *input.frame);
      if (cacheUpdated) {
        renderFromCache = input.gpuRenderer->RenderFromCache(*input.frameCache,
                                                            *input.art, &gpuErr);
        if (!renderFromCache && hadCachedFrame) {
          keepPreviousFrame = true;
        }
      } else if (hadCachedFrame) {
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
    } else {
      emitWarning(input.warningSink,
                  gpuErr.empty() ? "GPU renderer temporarily unavailable."
                                 : gpuErr);
      if (input.haveFrame) {
        *input.haveFrame = false;
      }
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

  if (input.cachedWidth) *input.cachedWidth = input.width;
  if (input.cachedMaxHeight) *input.cachedMaxHeight = input.maxHeight;
  if (input.cachedFrameWidth) *input.cachedFrameWidth = input.frame->width;
  if (input.cachedFrameHeight) *input.cachedFrameHeight = input.frame->height;
  if (input.cachedCellPixelWidth) {
    *input.cachedCellPixelWidth = input.cellPixelWidth;
  }
  if (input.cachedCellPixelHeight) {
    *input.cachedCellPixelHeight = input.cellPixelHeight;
  }
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
