#include "video_enhancement_backend.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdint>
#include <limits>

#include <d3d11.h>
#include <wrl/client.h>

namespace playback_video_enhancement {
namespace {

using Microsoft::WRL::ComPtr;

constexpr GUID kNvidiaPpeInterfaceGuid = {
    0xd43ce1b3,
    0x1f4b,
    0x48ac,
    {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
constexpr int64_t kFrameTimestampUnitsPerSecond = 10000000;

struct TargetSize {
  int width = 0;
  int height = 0;
};

int floorEven(int value) {
  if (value <= 2) {
    return 2;
  }
  return value - (value % 2);
}

TargetSize scaledTarget(int inputWidth, int inputHeight, double scale) {
  if (inputWidth <= 0 || inputHeight <= 0 || scale <= 1.0) {
    return {};
  }

  TargetSize target;
  target.width = floorEven(static_cast<int>(inputWidth * scale));
  target.height = floorEven(static_cast<int>(inputHeight * scale));
  if (target.width <= inputWidth && target.height <= inputHeight) {
    return {};
  }
  return target;
}

TargetSize fitUpscaleTarget(int inputWidth, int inputHeight, int targetWidth,
                            int targetHeight) {
  if (inputWidth <= 0 || inputHeight <= 0 || targetWidth <= 0 ||
      targetHeight <= 0) {
    return {};
  }

  const double scaleX = static_cast<double>(targetWidth) / inputWidth;
  const double scaleY = static_cast<double>(targetHeight) / inputHeight;
  const double scale = std::min(scaleX, scaleY);
  return scaledTarget(inputWidth, inputHeight, scale);
}

bool isVideoProcessorTextureFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

bool supportsVideoProcessorInputOutput(ID3D11Device* device,
                                       DXGI_FORMAT format) {
  UINT support = 0;
  if (FAILED(device->CheckFormatSupport(format, &support))) {
    return false;
  }
  constexpr UINT requiredSupport =
      D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT |
      D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
  return (support & requiredSupport) == requiredSupport;
}

DXGI_RATIONAL frameRateForFrame(const VideoFrame& frame) {
  DXGI_RATIONAL rate{1, 1};
  if (frame.duration100ns <= 0 ||
      frame.duration100ns > std::numeric_limits<UINT>::max()) {
    return rate;
  }

  rate.Numerator = static_cast<UINT>(kFrameTimestampUnitsPerSecond);
  rate.Denominator = static_cast<UINT>(frame.duration100ns);
  return rate;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpaceForFrame(const VideoFrame& frame) {
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpace{};
  colorSpace.YCbCr_Matrix = frame.yuvMatrix == YuvMatrix::Bt601 ? 0 : 1;
  colorSpace.Nominal_Range = frame.fullRange ? 2 : 1;
  return colorSpace;
}

class NvidiaD3D11VideoProcessorBackend final
    : public VideoEnhancementBackendState {
 public:
  ~NvidiaD3D11VideoProcessorBackend() override = default;

  const char* name() const override {
    return "nvidia-d3d11-video-processor";
  }

  VideoEnhancementBackend backend() const override {
    return VideoEnhancementBackend::NvidiaD3D11VideoProcessor;
  }

  bool process(const VideoEnhancementRequest& request,
               VideoEnhancementResult& result) override {
    auto fail = [&](const char* reason) {
      result.debugLine =
          buildVideoEnhancementDebugLine(request, name(), false, reason);
      return false;
    };

    if (!request.input) {
      return fail("no_input");
    }
    if (!request.device || !request.context) {
      return fail("no_d3d_context");
    }
    if (request.input->format != VideoPixelFormat::HWTexture) {
      return fail("not_hw_texture");
    }
    if (!request.input->hwTexture) {
      return fail("no_hw_texture");
    }
    if (!request.frameChanged && !request.forceRefresh) {
      return false;
    }

    D3D11_TEXTURE2D_DESC inputDesc{};
    request.input->hwTexture->GetDesc(&inputDesc);
    if (!isVideoProcessorTextureFormat(inputDesc.Format)) {
      return fail("unsupported_texture_format");
    }
    if (!supportsVideoProcessorInputOutput(request.device, inputDesc.Format)) {
      return fail("video_processor_format_unsupported");
    }

    const TargetSize target = fitUpscaleTarget(
        request.input->width, request.input->height, request.targetWidth,
        request.targetHeight);
    if (target.width <= 0 || target.height <= 0) {
      return fail("no_upscale_target");
    }

    if (!ensureDevice(request.device, request.context)) {
      return fail("ensure_device_failed");
    }
    if (!ensureProcessor(*request.input, inputDesc, target)) {
      return fail("ensure_processor_failed");
    }
    if (!ensureOutputTexture(inputDesc, target)) {
      return fail("ensure_output_texture_failed");
    }
    if (!render(*request.input)) {
      return fail("render_failed");
    }

    enhancedFrame_ = *request.input;
    enhancedFrame_.width = target.width;
    enhancedFrame_.height = target.height;
    enhancedFrame_.format = VideoPixelFormat::HWTexture;
    enhancedFrame_.hwTexture = outputTexture_;
    enhancedFrame_.hwTextureArrayIndex = 0;
    enhancedFrame_.hwFrameRef.reset();
    enhancedFrame_.rgba.clear();
    enhancedFrame_.yuv.clear();

    result.frame = &enhancedFrame_;
    result.frameAvailable = true;
    result.frameChanged = true;
    result.enhanced = true;
    result.backend = backend();
    result.debugLine =
        buildVideoEnhancementDebugLine(request, name(), true, "upscaled");
    return true;
  }

 private:
  bool ensureDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (device_.Get() == device && videoDevice_ && videoContext_) {
      return true;
    }

    reset();
    device_ = device;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(
        videoDevice_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
      reset();
      return false;
    }

    hr = context->QueryInterface(IID_PPV_ARGS(
        videoContext_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
      reset();
      return false;
    }
    return true;
  }

  bool ensureProcessor(const VideoFrame& frame,
                       const D3D11_TEXTURE2D_DESC& inputDesc,
                       TargetSize target) {
    if (videoProcessor_ && processorInputWidth_ == frame.width &&
        processorInputHeight_ == frame.height &&
        processorOutputWidth_ == target.width &&
        processorOutputHeight_ == target.height &&
        processorFormat_ == inputDesc.Format) {
      return nvidiaExtensionEnabled_;
    }

    videoProcessor_.Reset();
    processorEnumerator_.Reset();
    nvidiaExtensionEnabled_ = false;
    outputSequence_ = 0;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    const DXGI_RATIONAL frameRate = frameRateForFrame(frame);
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = frameRate;
    content.InputWidth = static_cast<UINT>(frame.width);
    content.InputHeight = static_cast<UINT>(frame.height);
    content.OutputFrameRate = frameRate;
    content.OutputWidth = static_cast<UINT>(target.width);
    content.OutputHeight = static_cast<UINT>(target.height);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(
        &content, processorEnumerator_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }

    hr = videoDevice_->CreateVideoProcessor(processorEnumerator_.Get(), 0,
                                            videoProcessor_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }

    RECT sourceRect{0, 0, static_cast<LONG>(frame.width),
                    static_cast<LONG>(frame.height)};
    RECT targetRect{0, 0, static_cast<LONG>(target.width),
                    static_cast<LONG>(target.height)};
    videoContext_->VideoProcessorSetStreamSourceRect(
        videoProcessor_.Get(), 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetOutputTargetRect(
        videoProcessor_.Get(), TRUE, &targetRect);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(
        videoProcessor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(
        videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamOutputRate(
        videoProcessor_.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL,
        FALSE, 0);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpace = colorSpaceForFrame(frame);
    videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0,
                                                     &colorSpace);
    videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(),
                                                     &colorSpace);

    struct NvidiaExtension {
      unsigned int version;
      unsigned int method;
      unsigned int enable;
    };
    NvidiaExtension extension{1, 2, 1};
    hr = videoContext_->VideoProcessorSetStreamExtension(
        videoProcessor_.Get(), 0, &kNvidiaPpeInterfaceGuid,
        sizeof(extension), &extension);
    if (FAILED(hr)) {
      videoProcessor_.Reset();
      processorEnumerator_.Reset();
      return false;
    }

    processorInputWidth_ = frame.width;
    processorInputHeight_ = frame.height;
    processorOutputWidth_ = target.width;
    processorOutputHeight_ = target.height;
    processorFormat_ = inputDesc.Format;
    nvidiaExtensionEnabled_ = true;
    return true;
  }

  bool ensureOutputTexture(const D3D11_TEXTURE2D_DESC& inputDesc,
                           TargetSize target) {
    if (outputTexture_ && outputWidth_ == target.width &&
        outputHeight_ == target.height && outputFormat_ == inputDesc.Format) {
      return true;
    }

    outputTexture_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(target.width);
    desc.Height = static_cast<UINT>(target.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = inputDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr,
                                          outputTexture_.GetAddressOf());
    if (FAILED(hr)) {
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      hr = device_->CreateTexture2D(&desc, nullptr,
                                    outputTexture_.GetAddressOf());
    }
    if (FAILED(hr)) {
      outputTexture_.Reset();
      return false;
    }

    outputWidth_ = target.width;
    outputHeight_ = target.height;
    outputFormat_ = inputDesc.Format;
    return true;
  }

  bool render(const VideoFrame& frame) {
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.ArraySlice =
        static_cast<UINT>(std::max(0, frame.hwTextureArrayIndex));
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        frame.hwTexture.Get(), processorEnumerator_.Get(), &inputViewDesc,
        inputView.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }

    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(
        outputTexture_.Get(), processorEnumerator_.Get(), &outputViewDesc,
        outputView.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    const UINT outputSequence = outputSequence_++;
    stream.InputFrameOrField = outputSequence;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = inputView.Get();

    hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(),
                                          outputView.Get(), outputSequence, 1,
                                          &stream);
    return SUCCEEDED(hr);
  }

  void reset() {
    outputTexture_.Reset();
    videoProcessor_.Reset();
    processorEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    device_.Reset();
    processorInputWidth_ = 0;
    processorInputHeight_ = 0;
    processorOutputWidth_ = 0;
    processorOutputHeight_ = 0;
    processorFormat_ = DXGI_FORMAT_UNKNOWN;
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;
    nvidiaExtensionEnabled_ = false;
    outputSequence_ = 0;
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11VideoDevice> videoDevice_;
  ComPtr<ID3D11VideoContext> videoContext_;
  ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator_;
  ComPtr<ID3D11VideoProcessor> videoProcessor_;
  ComPtr<ID3D11Texture2D> outputTexture_;
  VideoFrame enhancedFrame_;
  int processorInputWidth_ = 0;
  int processorInputHeight_ = 0;
  int processorOutputWidth_ = 0;
  int processorOutputHeight_ = 0;
  DXGI_FORMAT processorFormat_ = DXGI_FORMAT_UNKNOWN;
  int outputWidth_ = 0;
  int outputHeight_ = 0;
  DXGI_FORMAT outputFormat_ = DXGI_FORMAT_UNKNOWN;
  bool nvidiaExtensionEnabled_ = false;
  UINT outputSequence_ = 0;
};

}  // namespace

std::unique_ptr<VideoEnhancementBackendState>
createNvidiaD3D11VideoProcessorBackend() {
  return std::make_unique<NvidiaD3D11VideoProcessorBackend>();
}

}  // namespace playback_video_enhancement

#else

namespace playback_video_enhancement {

std::unique_ptr<VideoEnhancementBackendState>
createNvidiaD3D11VideoProcessorBackend() {
  return nullptr;
}

}  // namespace playback_video_enhancement

#endif
