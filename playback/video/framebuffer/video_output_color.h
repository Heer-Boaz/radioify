#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>

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

enum class HdrGenerateResult : uint32_t {
    Hdr10Generate = 0,
    ScRgbGenerate = 1,
    SdrFallback = 2,
    HardFailure = 3,
};

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
    bool outputSdrWhiteNitsAvailable = false;
    bool outputFound = false;
    bool monitorMatched = false;
    bool dxgiDescAvailable = false;
    DXGI_COLOR_SPACE_TYPE monitorColorSpace =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool advancedColorInfoAvailable = false;
    bool advancedColorSupported = false;
    bool advancedColorEnabled = false;
    bool advancedColorForceDisabled = false;
    uint32_t colorEncoding = 0;
    uint32_t bitsPerColorChannel = 0;
};

struct HdrGenerateDecision {
    HdrGenerateResult result = HdrGenerateResult::SdrFallback;
    VideoOutputColorState state;
    bool generatingHdr = false;
    std::string reason;
};

HdrGenerateDecision ResolveHdrGenerateOutputState(HWND hwnd,
                                                  IDXGIAdapter* adapter);
VideoOutputColorState VideoOutputScRgbGenerateState(
    const VideoOutputColorState& state);
VideoOutputColorState VideoOutputSdrFallbackState(
    const VideoOutputColorState& state);
bool VideoOutputUsesHdr(const VideoOutputColorState& state);
bool ApplyVideoOutputColorSpace(IDXGISwapChain& swapChain,
                                const VideoOutputColorState& state);
const char* VideoOutputColorEncodingName(VideoOutputColorEncoding encoding);
const char* HdrGenerateResultName(HdrGenerateResult result);
std::string VideoOutputColorAttemptStatus(
    const HdrGenerateDecision& decision,
    const VideoOutputColorState& selectedState,
    const char* stage);
std::string VideoOutputColorStateDebugLine(
    const VideoOutputColorState& state,
    const std::string& attemptStatus = {});
