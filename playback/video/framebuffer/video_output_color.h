#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <dxgi1_6.h>

enum class VideoOutputColorEncoding : uint32_t {
    Sdr = 0,
    ScRgb = 1,
    Hdr10 = 2,
};

static_assert(static_cast<uint32_t>(VideoOutputColorEncoding::Sdr) == 0);
static_assert(static_cast<uint32_t>(VideoOutputColorEncoding::ScRgb) == 1);
static_assert(static_cast<uint32_t>(VideoOutputColorEncoding::Hdr10) == 2);

inline constexpr float kVideoOutputStandardSdrWhiteNits = 80.0f;
inline constexpr float kVideoOutputBt2408ReferenceWhiteNits = 203.0f;
inline constexpr float kVideoOutputAsciiGlyphSdrWhiteScale = 3.5f;
inline constexpr float kVideoOutputAsciiGlyphFullFrameScale = 1.35f;

struct VideoOutputColorState {
    DXGI_FORMAT swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    VideoOutputColorEncoding encoding = VideoOutputColorEncoding::Sdr;
    float outputSdrWhiteNits = kVideoOutputStandardSdrWhiteNits;
    float outputPeakNits = 0.0f;
    float outputFullFrameNits = 0.0f;
    float asciiGlyphPeakNits = kVideoOutputStandardSdrWhiteNits;
};

struct VideoOutputDisplayInfo {
    float outputSdrWhiteNits = kVideoOutputStandardSdrWhiteNits;
    float outputPeakNits = 0.0f;
    float outputFullFrameNits = 0.0f;
    bool outputSdrWhiteNitsAvailable = false;
    bool outputFound = false;
    bool monitorMatched = false;
    bool dxgiDescAvailable = false;
    DXGI_COLOR_SPACE_TYPE monitorColorSpace =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

struct VideoOutputColorPlan {
    VideoOutputDisplayInfo display;
    std::vector<VideoOutputColorState> candidates;
    std::string reason;
};

VideoOutputColorPlan PlanVideoOutputColorsForSwapChain(
    IDXGISwapChain& swapChain);
std::vector<VideoOutputColorState> VideoOutputColorCandidatesForDisplay(
    const VideoOutputDisplayInfo& display,
    std::string* reason);
VideoOutputColorState VideoOutputHdr10ColorState(
    const VideoOutputDisplayInfo& display);
VideoOutputColorState VideoOutputScRgbColorState(
    const VideoOutputDisplayInfo& display);
VideoOutputColorState VideoOutputSdrColorState(
    const VideoOutputDisplayInfo& display);
bool VideoOutputUsesHdr(const VideoOutputColorState& state);
bool VideoOutputDisplayUsesHdr(const VideoOutputDisplayInfo& display);
bool ApplyVideoOutputColorSpace(IDXGISwapChain& swapChain,
                                const VideoOutputColorState& state);
bool ConfigureVideoOutputSwapChain(IDXGISwapChain& swapChain,
                                   int width,
                                   int height,
                                   UINT bufferFlags,
                                   VideoOutputColorState* selectedState,
                                   std::string* attemptStatus);
const char* VideoOutputColorEncodingName(VideoOutputColorEncoding encoding);
std::string VideoOutputColorAttemptStatus(
    const VideoOutputColorPlan& plan,
    const VideoOutputColorState& selectedState,
    const char* stage);
std::string VideoOutputColorStateDebugLine(
    const VideoOutputColorState& state,
    const std::string& attemptStatus = {});
