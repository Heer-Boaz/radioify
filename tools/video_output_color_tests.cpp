#include "playback/video/framebuffer/video_output_color.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "video_output_color_tests: " << message << '\n';
        return false;
    }
    return true;
}

VideoOutputDisplayInfo measuredDisplay() {
    VideoOutputDisplayInfo display{};
    display.outputFound = true;
    display.dxgiDescAvailable = true;
    display.outputSdrWhiteNitsAvailable = true;
    display.outputSdrWhiteNits = 200.0f;
    display.outputPeakNits = 1000.0f;
    display.outputFullFrameNits = 400.0f;
    return display;
}

}  // namespace

int main() {
    bool ok = true;

    VideoOutputDisplayInfo sdr = measuredDisplay();
    sdr.monitorColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    ok &= expect(!VideoOutputDisplayUsesHdr(sdr),
                 "SDR output color space must select SDR output");

    VideoOutputDisplayInfo hdr10 = measuredDisplay();
    hdr10.monitorColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    ok &= expect(VideoOutputDisplayUsesHdr(hdr10),
                 "HDR10 output color space must select HDR output");
    VideoOutputColorState hdr10Output = VideoOutputHdr10ColorState(hdr10);
    ok &= expect(hdr10Output.swapChainFormat == DXGI_FORMAT_R10G10B10A2_UNORM,
                 "HDR10 output must use RGB10A2 swapchain format");
    ok &= expect(hdr10Output.colorSpace ==
                     DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                 "HDR10 output must use the HDR10 DXGI color space");
    std::string hdr10Reason;
    std::vector<VideoOutputColorState> hdr10Candidates =
        VideoOutputColorCandidatesForDisplay(hdr10, &hdr10Reason);
    ok &= expect(hdr10Candidates.size() == 2,
                 "HDR10 output plan must contain HDR10 and SDR candidates");
    ok &= expect(hdr10Candidates.front().encoding ==
                     VideoOutputColorEncoding::Hdr10,
                 "HDR10 output plan must prefer native HDR10");
    ok &= expect(hdr10Candidates.back().encoding ==
                     VideoOutputColorEncoding::Sdr,
                 "HDR10 output plan must keep SDR as the final fallback");

    VideoOutputDisplayInfo unmeasuredHdr10 = hdr10;
    unmeasuredHdr10.outputSdrWhiteNitsAvailable = false;
    unmeasuredHdr10.outputSdrWhiteNits = 0.0f;
    unmeasuredHdr10.outputPeakNits = 0.0f;
    unmeasuredHdr10.outputFullFrameNits = 0.0f;
    std::vector<VideoOutputColorState> unmeasuredHdr10Candidates =
        VideoOutputColorCandidatesForDisplay(unmeasuredHdr10, nullptr);
    ok &= expect(unmeasuredHdr10Candidates.front().encoding ==
                     VideoOutputColorEncoding::Hdr10,
                 "Missing luminance metadata must not change HDR10 output planning");

    VideoOutputDisplayInfo scrgb = measuredDisplay();
    scrgb.monitorColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    ok &= expect(VideoOutputDisplayUsesHdr(scrgb),
                 "scRGB output color space must select HDR output");
    VideoOutputColorState scrgbOutput = VideoOutputScRgbColorState(scrgb);
    ok &= expect(scrgbOutput.swapChainFormat ==
                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                 "scRGB output must use float16 swapchain format");
    ok &= expect(scrgbOutput.colorSpace ==
                     DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                 "scRGB output must use the scRGB DXGI color space");

    return ok ? 0 : 1;
}
