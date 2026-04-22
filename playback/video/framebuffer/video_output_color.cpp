#include "video_output_color.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kStandardSdrWhiteNits = 80.0f;

LONG64 intersectionArea(const RECT& a, const RECT& b) {
    const LONG left = std::max(a.left, b.left);
    const LONG top = std::max(a.top, b.top);
    const LONG right = std::min(a.right, b.right);
    const LONG bottom = std::min(a.bottom, b.bottom);
    if (right <= left || bottom <= top) {
        return 0;
    }
    return static_cast<LONG64>(right - left) *
           static_cast<LONG64>(bottom - top);
}

bool sameGdiDeviceName(const WCHAR* lhs, const WCHAR* rhs) {
    return lhs && rhs && _wcsicmp(lhs, rhs) == 0;
}

float querySdrWhiteNitsForGdiDeviceName(const WCHAR* gdiDeviceName) {
    if (!gdiDeviceName || gdiDeviceName[0] == L'\0') {
        return kStandardSdrWhiteNits;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    &modeCount) != ERROR_SUCCESS ||
        pathCount == 0 || modeCount == 0) {
        return kStandardSdrWhiteNits;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG queryResult = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
        modes.data(), nullptr);
    if (queryResult != ERROR_SUCCESS) {
        return kStandardSdrWhiteNits;
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
            return (static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0f) *
                   kStandardSdrWhiteNits;
        }
    }

    return kStandardSdrWhiteNits;
}

void queryAdvancedColorForGdiDeviceName(const WCHAR* gdiDeviceName,
                                        VideoOutputColorState& state) {
    if (!gdiDeviceName || gdiDeviceName[0] == L'\0') {
        return;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    &modeCount) != ERROR_SUCCESS ||
        pathCount == 0 || modeCount == 0) {
        return;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG queryResult = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
        modes.data(), nullptr);
    if (queryResult != ERROR_SUCCESS) {
        return;
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

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo{};
        colorInfo.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        colorInfo.header.size = sizeof(colorInfo);
        colorInfo.header.adapterId = path.targetInfo.adapterId;
        colorInfo.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&colorInfo.header) == ERROR_SUCCESS) {
            state.advancedColorInfoAvailable = true;
            state.advancedColorSupported = colorInfo.advancedColorSupported != 0;
            state.advancedColorEnabled = colorInfo.advancedColorEnabled != 0;
            state.advancedColorForceDisabled =
                colorInfo.advancedColorForceDisabled != 0;
            state.colorEncoding = static_cast<uint32_t>(colorInfo.colorEncoding);
            state.bitsPerColorChannel = colorInfo.bitsPerColorChannel;
        }
        return;
    }
}

bool outputIsHdrActive(const DXGI_OUTPUT_DESC1& desc) {
    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
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

struct OutputSelection {
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    LONG64 area = -1;
    bool monitorMatch = false;
};

void considerOutputForWindow(IDXGIOutput* output, HMONITOR targetMonitor,
                             const RECT& windowRect,
                             OutputSelection& selection) {
    if (!output) {
        return;
    }

    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) {
        return;
    }

    const bool monitorMatch =
        targetMonitor != nullptr && desc.Monitor == targetMonitor;
    const LONG64 area = intersectionArea(windowRect, desc.DesktopCoordinates);
    const bool better =
        !selection.output ||
        (monitorMatch && !selection.monitorMatch) ||
        (monitorMatch == selection.monitorMatch && area > selection.area);
    if (!better) {
        return;
    }

    selection.output = output;
    selection.desc = desc;
    selection.area = area;
    selection.monitorMatch = monitorMatch;
}

void enumerateAdapterOutputsForWindow(IDXGIAdapter* adapter,
                                      HMONITOR targetMonitor,
                                      const RECT& windowRect,
                                      OutputSelection& selection) {
    if (!adapter) {
        return;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        const HRESULT hr = adapter->EnumOutputs(index, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }
        considerOutputForWindow(output.Get(), targetMonitor, windowRect,
                                selection);
    }
}

OutputSelection chooseOutputForWindow(HWND hwnd, IDXGIAdapter* adapter) {
    OutputSelection selection{};
    HMONITOR targetMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (targetMonitor &&
            GetMonitorInfoW(targetMonitor, &monitorInfo)) {
            windowRect = monitorInfo.rcMonitor;
        }
    }

    ComPtr<IDXGIFactory> factory;
    if (adapter &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory) {
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter> candidateAdapter;
            const HRESULT hr =
                factory->EnumAdapters(adapterIndex, &candidateAdapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }
            enumerateAdapterOutputsForWindow(candidateAdapter.Get(),
                                             targetMonitor, windowRect,
                                             selection);
        }
    }

    if (!selection.output) {
        enumerateAdapterOutputsForWindow(adapter, targetMonitor, windowRect,
                                         selection);
    }

    return selection;
}

}  // namespace

VideoOutputColorState ChooseVideoOutputColorState(HWND hwnd,
                                                  IDXGIAdapter* adapter) {
    VideoOutputColorState state{};
    if (!hwnd || !adapter) {
        return state;
    }

    const OutputSelection outputSelection =
        chooseOutputForWindow(hwnd, adapter);
    if (!outputSelection.output) {
        return state;
    }
    state.outputFound = true;
    state.monitorMatched = outputSelection.monitorMatch;
    state.sdrWhiteNits =
        querySdrWhiteNitsForGdiDeviceName(outputSelection.desc.DeviceName);
    queryAdvancedColorForGdiDeviceName(outputSelection.desc.DeviceName, state);

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(outputSelection.output.As(&output6)) || !output6) {
        return state;
    }

    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output6->GetDesc1(&desc1))) {
        return state;
    }
    state.dxgiDescAvailable = true;
    state.monitorColorSpace = desc1.ColorSpace;
    state.maxOutputNits =
        std::max(desc1.MaxFullFrameLuminance, desc1.MaxLuminance);

    if (!outputIsHdrActive(desc1)) {
        return state;
    }

    state.swapChainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    state.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    state.encoding = VideoOutputColorEncoding::Hdr10;
    return state;
}

VideoOutputColorState VideoOutputScRgbFallbackState(
    const VideoOutputColorState& state) {
    VideoOutputColorState fallback = state;
    fallback.swapChainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fallback.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    fallback.encoding = VideoOutputColorEncoding::ScRgb;
    return fallback;
}

bool VideoOutputUsesHdr(const VideoOutputColorState& state) {
    return state.encoding != VideoOutputColorEncoding::Sdr;
}

bool ApplyVideoOutputColorSpace(IDXGISwapChain* swapChain,
                                const VideoOutputColorState& state) {
    if (!swapChain) {
        return false;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) ||
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

float VideoOutputSdrWhiteScale(const VideoOutputColorState& state) {
    if (!VideoOutputUsesHdr(state) || !std::isfinite(state.sdrWhiteNits) ||
        state.sdrWhiteNits <= 0.0f) {
        return 1.0f;
    }
    return state.sdrWhiteNits / kStandardSdrWhiteNits;
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

std::string VideoOutputColorStateDebugLine(
    const VideoOutputColorState& state,
    const std::string& attemptStatus) {
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "DBG hdr out=%s fmt=%s set=%s mon=%s found=%d match=%d dxgi=%d aci=%d ac=%d/%d force=%d bpc=%u white=%.0f max=%.0f",
        VideoOutputColorEncodingName(state.encoding),
        dxgiFormatName(state.swapChainFormat), colorSpaceName(state.colorSpace),
        state.dxgiDescAvailable ? colorSpaceName(state.monitorColorSpace)
                                : "unknown",
        state.outputFound ? 1 : 0, state.monitorMatched ? 1 : 0,
        state.dxgiDescAvailable ? 1 : 0,
        state.advancedColorInfoAvailable ? 1 : 0,
        state.advancedColorSupported ? 1 : 0,
        state.advancedColorEnabled ? 1 : 0,
        state.advancedColorForceDisabled ? 1 : 0,
        state.bitsPerColorChannel, state.sdrWhiteNits, state.maxOutputNits);
    if (attemptStatus.empty()) {
        return std::string(buf);
    }
    return std::string(buf) + " " + attemptStatus;
}
