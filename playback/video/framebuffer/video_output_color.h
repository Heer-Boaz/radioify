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

struct VideoOutputColorState {
    DXGI_FORMAT swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    VideoOutputColorEncoding encoding = VideoOutputColorEncoding::Sdr;
    float sdrWhiteNits = 80.0f;
    float maxOutputNits = 0.0f;
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

VideoOutputColorState ChooseVideoOutputColorState(HWND hwnd,
                                                  IDXGIAdapter* adapter,
                                                  bool preferHdrOutput);
VideoOutputColorState VideoOutputScRgbFallbackState(
    const VideoOutputColorState& state);
bool VideoOutputUsesHdr(const VideoOutputColorState& state);
bool ApplyVideoOutputColorSpace(IDXGISwapChain* swapChain,
                                const VideoOutputColorState& state);
float VideoOutputSdrWhiteScale(const VideoOutputColorState& state);
const char* VideoOutputColorEncodingName(VideoOutputColorEncoding encoding);
std::string VideoOutputColorStateDebugLine(
    const VideoOutputColorState& state,
    const std::string& attemptStatus = {});
