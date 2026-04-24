#include "video_output_color.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

bool sameGdiDeviceName(const WCHAR* lhs, const WCHAR* rhs) {
    return lhs && rhs && _wcsicmp(lhs, rhs) == 0;
}

bool querySdrWhiteNitsForGdiDeviceName(const WCHAR* gdiDeviceName,
                                       float* outNits) {
    if (!gdiDeviceName || gdiDeviceName[0] == L'\0') {
        return false;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    &modeCount) != ERROR_SUCCESS ||
        pathCount == 0 || modeCount == 0) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG queryResult = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
        modes.data(), nullptr);
    if (queryResult != ERROR_SUCCESS) {
        return false;
    }

    paths.resize(pathCount);
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
            continue;
        }
        if (!sameGdiDeviceName(sourceName.viewGdiDeviceName, gdiDeviceName)) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
        whiteLevel.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        whiteLevel.header.size = sizeof(whiteLevel);
        whiteLevel.header.adapterId = path.targetInfo.adapterId;
        whiteLevel.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS &&
            whiteLevel.SDRWhiteLevel > 0) {
            if (outNits) {
                *outNits =
                    (static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0f) *
                    kVideoOutputStandardSdrWhiteNits;
            }
            return true;
        }
    }

    return false;
}

const char* colorSpaceName(DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            return "HDR10/PQ";
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return "scRGB";
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
            return "SDR";
        default:
            return "other";
    }
}

const char* dxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return "R10G10B10A2";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return "R16G16B16A16F";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8";
        default:
            return "other";
    }
}

VideoOutputDisplayInfo probeVideoOutputSwapChain(IDXGISwapChain& swapChain) {
    VideoOutputDisplayInfo info{};

    ComPtr<IDXGIOutput> output;
    if (FAILED(swapChain.GetContainingOutput(&output)) || !output) {
        return info;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    if (SUCCEEDED(output->GetDesc(&outputDesc))) {
        info.outputFound = true;
        float sdrWhiteNits = 0.0f;
        info.outputSdrWhiteNitsAvailable =
            querySdrWhiteNitsForGdiDeviceName(outputDesc.DeviceName,
                                              &sdrWhiteNits);
        if (info.outputSdrWhiteNitsAvailable) {
            info.outputSdrWhiteNits = sdrWhiteNits;
        }
    }

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6)) || !output6) {
        return info;
    }

    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output6->GetDesc1(&desc1))) {
        return info;
    }
    info.outputFound = true;
    info.dxgiDescAvailable = true;
    info.monitorColorSpace = desc1.ColorSpace;
    info.outputPeakNits = desc1.MaxLuminance;
    info.outputFullFrameNits = desc1.MaxFullFrameLuminance;
    return info;
}

float resolvedSdrWhiteNits(const VideoOutputDisplayInfo& display) {
    if (display.outputSdrWhiteNitsAvailable &&
        std::isfinite(display.outputSdrWhiteNits) &&
        display.outputSdrWhiteNits > 0.0f) {
        return display.outputSdrWhiteNits;
    }
    return kVideoOutputStandardSdrWhiteNits;
}

float resolvedOutputPeakNits(const VideoOutputDisplayInfo& display) {
    if (std::isfinite(display.outputPeakNits) &&
        display.outputPeakNits > 0.0f) {
        return display.outputPeakNits;
    }
    return resolvedSdrWhiteNits(display);
}

float resolvedOutputFullFrameNits(const VideoOutputDisplayInfo& display) {
    if (std::isfinite(display.outputFullFrameNits) &&
        display.outputFullFrameNits > 0.0f) {
        return display.outputFullFrameNits;
    }
    return resolvedOutputPeakNits(display);
}

float resolveAsciiGlyphPeakNits(const VideoOutputDisplayInfo& display) {
    const float sdrWhiteNits = resolvedSdrWhiteNits(display);
    const float outputPeakNits = resolvedOutputPeakNits(display);
    const float outputFullFrameNits = resolvedOutputFullFrameNits(display);
    const float diffuseWhite = std::max(kVideoOutputBt2408ReferenceWhiteNits,
                                        sdrWhiteNits);
    const float policyTarget =
        std::max(diffuseWhite *
                     kVideoOutputAsciiGlyphSdrWhiteScale,
                 outputFullFrameNits *
                     kVideoOutputAsciiGlyphFullFrameScale);
    return std::min(outputPeakNits,
                    std::max(diffuseWhite + 1.0f, policyTarget));
}

VideoOutputColorState videoOutputColorStateFromDisplay(
    const VideoOutputDisplayInfo& display) {
    VideoOutputColorState output{};
    output.outputSdrWhiteNits = resolvedSdrWhiteNits(display);
    output.outputPeakNits = resolvedOutputPeakNits(display);
    output.outputFullFrameNits = resolvedOutputFullFrameNits(display);
    output.asciiGlyphPeakNits = resolveAsciiGlyphPeakNits(display);
    return output;
}

}  // namespace

bool VideoOutputDisplayUsesHdr(const VideoOutputDisplayInfo& display) {
    switch (display.monitorColorSpace) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return true;
        default:
            return false;
    }
}

VideoOutputColorPlan PlanVideoOutputColorsForSwapChain(
    IDXGISwapChain& swapChain) {
    VideoOutputDisplayInfo display = probeVideoOutputSwapChain(swapChain);
    VideoOutputColorPlan plan;
    plan.display = display;
    plan.candidates =
        VideoOutputColorCandidatesForDisplay(display, &plan.reason);
    return plan;
}

std::vector<VideoOutputColorState> VideoOutputColorCandidatesForDisplay(
    const VideoOutputDisplayInfo& display,
    std::string* reason) {
    VideoOutputDisplayInfo measured = display;
    std::vector<VideoOutputColorState> candidates;
    if (!measured.outputFound) {
        candidates.push_back(VideoOutputSdrColorState(measured));
        if (reason) *reason = "output_not_found";
        return candidates;
    }
    if (!measured.dxgiDescAvailable) {
        candidates.push_back(VideoOutputSdrColorState(measured));
        if (reason) *reason = "dxgi_output6_unavailable";
        return candidates;
    }
    if (!VideoOutputDisplayUsesHdr(measured)) {
        candidates.push_back(VideoOutputSdrColorState(measured));
        if (reason) *reason = "output_color_space_sdr";
        return candidates;
    }

    if (measured.monitorColorSpace ==
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        candidates.push_back(VideoOutputHdr10ColorState(measured));
        candidates.push_back(VideoOutputSdrColorState(measured));
        if (reason) *reason = "hdr10_output_active";
        return candidates;
    }

    candidates.push_back(VideoOutputScRgbColorState(measured));
    candidates.push_back(VideoOutputSdrColorState(measured));
    if (reason) *reason = "scrgb_output_active";
    return candidates;
}

VideoOutputColorState VideoOutputHdr10ColorState(
    const VideoOutputDisplayInfo& display) {
    VideoOutputColorState output = videoOutputColorStateFromDisplay(display);
    output.swapChainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    output.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    output.encoding = VideoOutputColorEncoding::Hdr10;
    return output;
}

VideoOutputColorState VideoOutputScRgbColorState(
    const VideoOutputDisplayInfo& display) {
    VideoOutputColorState output = videoOutputColorStateFromDisplay(display);
    output.swapChainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    output.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    output.encoding = VideoOutputColorEncoding::ScRgb;
    return output;
}

VideoOutputColorState VideoOutputSdrColorState(
    const VideoOutputDisplayInfo& display) {
    VideoOutputColorState fallback = videoOutputColorStateFromDisplay(display);
    fallback.swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    fallback.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    fallback.encoding = VideoOutputColorEncoding::Sdr;
    return fallback;
}

bool VideoOutputUsesHdr(const VideoOutputColorState& state) {
    return state.encoding != VideoOutputColorEncoding::Sdr;
}

bool ApplyVideoOutputColorSpace(IDXGISwapChain& swapChain,
                                const VideoOutputColorState& state) {
    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain.QueryInterface(IID_PPV_ARGS(&swapChain3))) ||
        !swapChain3) {
        return !VideoOutputUsesHdr(state);
    }

    UINT support = 0;
    if (FAILED(swapChain3->CheckColorSpaceSupport(state.colorSpace,
                                                  &support)) ||
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        return !VideoOutputUsesHdr(state);
    }

    return SUCCEEDED(swapChain3->SetColorSpace1(state.colorSpace));
}

bool ConfigureVideoOutputSwapChain(IDXGISwapChain& swapChain,
                                   int width,
                                   int height,
                                   UINT bufferFlags,
                                   VideoOutputColorState* selectedState,
                                   std::string* attemptStatus) {
    VideoOutputColorPlan plan = PlanVideoOutputColorsForSwapChain(swapChain);
    std::string lastFailure;
    for (size_t index = 0; index < plan.candidates.size(); ++index) {
        const VideoOutputColorState& candidate = plan.candidates[index];
        HRESULT hr = swapChain.ResizeBuffers(
            0, static_cast<UINT>(width), static_cast<UINT>(height),
            candidate.swapChainFormat, bufferFlags);
        if (FAILED(hr)) {
            char status[96];
            std::snprintf(status, sizeof(status), "failed=resize_%s:0x%08X",
                          VideoOutputColorEncodingName(candidate.encoding),
                          static_cast<unsigned int>(hr));
            lastFailure = status;
            if (attemptStatus) *attemptStatus = lastFailure;
            continue;
        }
        if (!ApplyVideoOutputColorSpace(swapChain, candidate)) {
            lastFailure =
                std::string("failed=set_") +
                VideoOutputColorEncodingName(candidate.encoding);
            if (attemptStatus) *attemptStatus = lastFailure;
            continue;
        }

        if (selectedState) *selectedState = candidate;
        const char* stage = index == 0 ? "active" : "fallback";
        std::string status =
            VideoOutputColorAttemptStatus(plan, candidate, stage);
        if (!lastFailure.empty()) {
            status += " last_failure=" + lastFailure;
        }
        if (attemptStatus) *attemptStatus = status;
        return true;
    }

    if (attemptStatus && !lastFailure.empty()) {
        *attemptStatus = lastFailure;
    }
    return false;
}

const char* VideoOutputColorEncodingName(VideoOutputColorEncoding encoding) {
    switch (encoding) {
        case VideoOutputColorEncoding::Hdr10:
            return "HDR10";
        case VideoOutputColorEncoding::ScRgb:
            return "scRGB";
        case VideoOutputColorEncoding::Sdr:
        default:
            return "SDR";
    }
}

std::string VideoOutputColorAttemptStatus(
    const VideoOutputColorPlan& plan,
    const VideoOutputColorState& selectedState,
    const char* stage) {
    const float diffuseWhiteNits =
        std::max(kVideoOutputBt2408ReferenceWhiteNits,
                 selectedState.outputSdrWhiteNits);
    char buf[384];
    std::snprintf(
        buf, sizeof(buf),
        "policy=output_color selected=%s hdr=%d stage=%s monitor=%s paper=%.0f peak=%.0f fullframe=%.0f glyphPeak=%.0f diffuse=%.0f reason=%s",
        VideoOutputColorEncodingName(selectedState.encoding),
        VideoOutputUsesHdr(selectedState) ? 1 : 0,
        stage ? stage : "unknown",
        plan.display.dxgiDescAvailable
            ? colorSpaceName(plan.display.monitorColorSpace)
            : "unknown",
        selectedState.outputSdrWhiteNits,
        selectedState.outputPeakNits,
        selectedState.outputFullFrameNits,
        selectedState.asciiGlyphPeakNits,
        diffuseWhiteNits,
        plan.reason.empty() ? "unspecified" : plan.reason.c_str());
    return std::string(buf);
}

std::string VideoOutputColorStateDebugLine(
    const VideoOutputColorState& state,
    const std::string& attemptStatus) {
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "DBG hdr out=%s fmt=%s set=%s white=%.0f peak=%.0f full=%.0f glyph=%.0f",
        VideoOutputColorEncodingName(state.encoding),
        dxgiFormatName(state.swapChainFormat), colorSpaceName(state.colorSpace),
        state.outputSdrWhiteNits,
        state.outputPeakNits, state.outputFullFrameNits,
        state.asciiGlyphPeakNits);
    if (attemptStatus.empty()) {
        return std::string(buf);
    }
    return std::string(buf) + " " + attemptStatus;
}
