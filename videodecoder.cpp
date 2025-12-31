#include "videodecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

template <typename T>
void safeRelease(T*& ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

bool ensureMediaFoundation(std::string* error) {
  static bool started = false;
  static bool comStarted = false;
  if (started) return true;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) {
    comStarted = true;
  } else if (hr != RPC_E_CHANGED_MODE) {
    setError(error, "Failed to initialize COM for video decoding.");
    return false;
  }

  hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(hr)) {
    if (comStarted) {
      CoUninitialize();
      comStarted = false;
    }
    setError(error, "Failed to start Media Foundation for video decoding.");
    return false;
  }

  started = true;
  return true;
}
} // namespace

struct VideoDecoder::Impl {
  IMFSourceReader* reader = nullptr;
  IMFDXGIDeviceManager* dxgiManager = nullptr;
  ID3D11Device* d3dDevice = nullptr;
  ID3D11DeviceContext* d3dContext = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool topDown = true;
  bool atEnd = false;
  int64_t duration100ns = 0;
  int64_t lastTimestamp100ns = 0;
  int64_t lastDuration100ns = 0;
  int consecutiveErrors = 0;
  GUID subtype = GUID_NULL;
  UINT32 yuvMatrix = MFVideoTransferMatrix_BT709;
  bool fullRange = true;
};

VideoDecoder::~VideoDecoder() { uninit(); }

namespace {
enum class PixelFormat {
  RGB32,
  ARGB32,
  NV12,
  P010,
  Unknown,
};

PixelFormat pixelFormatFromSubtype(const GUID& subtype) {
  if (subtype == MFVideoFormat_RGB32) return PixelFormat::RGB32;
  if (subtype == MFVideoFormat_ARGB32) return PixelFormat::ARGB32;
  if (subtype == MFVideoFormat_NV12) return PixelFormat::NV12;
  if (subtype == MFVideoFormat_P010) return PixelFormat::P010;
  return PixelFormat::Unknown;
}

std::string guidToString(const GUID& guid) {
  char buf[64];
  std::snprintf(
      buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
      static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
      guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
      guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return std::string(buf);
}

std::string pixelFormatName(const GUID& subtype) {
  if (subtype == MFVideoFormat_RGB32) return "RGB32";
  if (subtype == MFVideoFormat_ARGB32) return "ARGB32";
  if (subtype == MFVideoFormat_NV12) return "NV12";
  if (subtype == MFVideoFormat_P010) return "P010";
  return guidToString(subtype);
}

std::string hrToString(HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return std::string(buf);
}

void updateColorInfo(IMFMediaType* mediaType, UINT32* yuvMatrix,
                     bool* fullRange) {
  if (!mediaType) return;
  UINT32 matrix = 0;
  if (yuvMatrix &&
      SUCCEEDED(mediaType->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
    *yuvMatrix = matrix;
  }
  UINT32 range = 0;
  if (fullRange &&
      SUCCEEDED(mediaType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) {
    *fullRange = (range == MFNominalRange_0_255);
  } else if (fullRange) {
    *fullRange = true;
  }
}

inline uint8_t clampToByte(float v) {
  if (v < 0.0f) return 0;
  if (v > 255.0f) return 255;
  return static_cast<uint8_t>(v + 0.5f);
}

void yuvToRgb(int y, int u, int v, bool fullRange, UINT32 matrix,
              uint8_t& outR, uint8_t& outG, uint8_t& outB) {
  float yf = 0.0f;
  float uf = 0.0f;
  float vf = 0.0f;
  if (fullRange) {
    yf = static_cast<float>(y) / 255.0f;
    uf = (static_cast<float>(u) - 128.0f) / 255.0f;
    vf = (static_cast<float>(v) - 128.0f) / 255.0f;
  } else {
    yf = (static_cast<float>(y) - 16.0f) / 219.0f;
    uf = (static_cast<float>(u) - 128.0f) / 224.0f;
    vf = (static_cast<float>(v) - 128.0f) / 224.0f;
  }

  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (matrix == MFVideoTransferMatrix_BT709) {
    r = yf + 1.5748f * vf;
    g = yf - 0.1873f * uf - 0.4681f * vf;
    b = yf + 1.8556f * uf;
  } else {
    r = yf + 1.4020f * vf;
    g = yf - 0.3441f * uf - 0.7141f * vf;
    b = yf + 1.7720f * uf;
  }

  outR = clampToByte(r * 255.0f);
  outG = clampToByte(g * 255.0f);
  outB = clampToByte(b * 255.0f);
}

bool createHardwareReaderAttributes(IMFAttributes** attributes,
                                    IMFDXGIDeviceManager** dxgiManager,
                                    ID3D11Device** d3dDevice,
                                    ID3D11DeviceContext** d3dContext) {
  if (!attributes || !dxgiManager || !d3dDevice || !d3dContext) {
    return false;
  }
  *attributes = nullptr;
  *dxgiManager = nullptr;
  *d3dDevice = nullptr;
  *d3dContext = nullptr;

  UINT creationFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                D3D_FEATURE_LEVEL_11_0,
                                D3D_FEATURE_LEVEL_10_1,
                                D3D_FEATURE_LEVEL_10_0};
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 creationFlags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &device, nullptr,
                                 &context);
  if (hr == E_INVALIDARG) {
    D3D_FEATURE_LEVEL fallbackLevels[] = {D3D_FEATURE_LEVEL_11_0,
                                          D3D_FEATURE_LEVEL_10_1,
                                          D3D_FEATURE_LEVEL_10_0};
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           creationFlags, fallbackLevels,
                           ARRAYSIZE(fallbackLevels), D3D11_SDK_VERSION,
                           &device, nullptr, &context);
  }
  if (FAILED(hr)) {
    return false;
  }

  ID3D11Multithread* multithread = nullptr;
  hr = context->QueryInterface(__uuidof(ID3D11Multithread),
                               reinterpret_cast<void**>(&multithread));
  if (SUCCEEDED(hr) && multithread) {
    multithread->SetMultithreadProtected(TRUE);
    multithread->Release();
  }

  IMFDXGIDeviceManager* manager = nullptr;
  UINT resetToken = 0;
  hr = MFCreateDXGIDeviceManager(&resetToken, &manager);
  if (FAILED(hr) || FAILED(manager->ResetDevice(device, resetToken))) {
    safeRelease(manager);
    safeRelease(context);
    safeRelease(device);
    return false;
  }

  IMFAttributes* attrs = nullptr;
  hr = MFCreateAttributes(&attrs, 5);
  if (FAILED(hr) ||
      FAILED(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager)) ||
      FAILED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                              TRUE)) ||
      FAILED(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE)) ||
      FAILED(attrs->SetUINT32(
          MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE)) ||
      FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                              FALSE))) {
    safeRelease(attrs);
    safeRelease(manager);
    safeRelease(context);
    safeRelease(device);
    return false;
  }

  *attributes = attrs;
  *dxgiManager = manager;
  *d3dDevice = device;
  *d3dContext = context;
  return true;
}

bool createSoftwareReaderAttributes(IMFAttributes** attributes) {
  if (!attributes) return false;
  *attributes = nullptr;
  IMFAttributes* attrs = nullptr;
  HRESULT hr = MFCreateAttributes(&attrs, 3);
  if (FAILED(hr)) {
    return false;
  }
  if (FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                              TRUE)) ||
      FAILED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                              FALSE)) ||
      FAILED(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE))) {
    safeRelease(attrs);
    return false;
  }
  *attributes = attrs;
  return true;
}
} // namespace

bool VideoDecoder::init(const std::filesystem::path& path, std::string* error) {
  uninit();

  if (!ensureMediaFoundation(error)) return false;

  IMFAttributes* attributes = nullptr;
  IMFDXGIDeviceManager* dxgiManager = nullptr;
  ID3D11Device* d3dDevice = nullptr;
  ID3D11DeviceContext* d3dContext = nullptr;
  bool hardwareEnabled = createHardwareReaderAttributes(
      &attributes, &dxgiManager, &d3dDevice, &d3dContext);
  if (!hardwareEnabled) {
    createSoftwareReaderAttributes(&attributes);
  }
  HRESULT hr = S_OK;
  auto releaseHardware = [&]() {
    safeRelease(dxgiManager);
    safeRelease(d3dContext);
    safeRelease(d3dDevice);
  };

  IMFSourceReader* reader = nullptr;
  std::wstring wpath = path.wstring();
  HRESULT hrPrimary = MFCreateSourceReaderFromURL(wpath.c_str(), attributes,
                                                  &reader);
  safeRelease(attributes);
  if (FAILED(hrPrimary) && hardwareEnabled) {
    safeRelease(reader);
    releaseHardware();
    hardwareEnabled = false;
    if (createSoftwareReaderAttributes(&attributes)) {
      hrPrimary =
          MFCreateSourceReaderFromURL(wpath.c_str(), attributes, &reader);
    }
    safeRelease(attributes);
  }
  if (FAILED(hrPrimary)) {
    IMFSourceReader* fallback = nullptr;
    HRESULT hrFallback =
        MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &fallback);
    if (FAILED(hrFallback)) {
      releaseHardware();
      std::string detail = "Failed to open video source (hr=" +
                           hrToString(hrPrimary) +
                           ", fallback hr=" + hrToString(hrFallback) + ").";
      setError(error, detail.c_str());
      return false;
    }
    reader = fallback;
  }

  hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  if (SUCCEEDED(hr)) {
    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  }
  if (FAILED(hr)) {
    safeRelease(reader);
    releaseHardware();
    std::string detail =
        "No video stream found (hr=" + hrToString(hr) + ").";
    setError(error, detail.c_str());
    return false;
  }

  IMFMediaType* nativeType = nullptr;
  hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                  &nativeType);
  safeRelease(nativeType);
  if (FAILED(hr)) {
    safeRelease(reader);
    releaseHardware();
    std::string detail =
        "No video stream found (hr=" + hrToString(hr) + ").";
    setError(error, detail.c_str());
    return false;
  }

  IMFMediaType* type = nullptr;
  hr = MFCreateMediaType(&type);
  if (FAILED(hr)) {
    safeRelease(reader);
    releaseHardware();
    std::string detail =
        "Failed to configure video decoding (hr=" + hrToString(hr) + ").";
    setError(error, detail.c_str());
    return false;
  }

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (SUCCEEDED(hr)) {
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                     nullptr, type);
  }
  safeRelease(type);
  if (FAILED(hr)) {
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_P010);
    if (SUCCEEDED(hr)) {
      hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       nullptr, type);
    }
    safeRelease(type);
  }
  if (FAILED(hr)) {
    safeRelease(reader);
    releaseHardware();
    setError(error,
             ("Failed to configure video output (hr=" + hrToString(hr) + ")")
                 .c_str());
    return false;
  }

  IMFMediaType* actualType = nullptr;
  hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   &actualType);
  if (FAILED(hr)) {
    safeRelease(reader);
    releaseHardware();
    std::string detail =
        "Failed to query video format (hr=" + hrToString(hr) + ").";
    setError(error, detail.c_str());
    return false;
  }

  GUID subtype = GUID_NULL;
  if (FAILED(actualType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
    safeRelease(actualType);
    safeRelease(reader);
    releaseHardware();
    setError(error, "Failed to read video pixel format.");
    return false;
  }
  PixelFormat format = pixelFormatFromSubtype(subtype);
  if (format == PixelFormat::Unknown) {
    std::string detail =
        "Unsupported video pixel format: " + pixelFormatName(subtype);
    safeRelease(actualType);
    safeRelease(reader);
    releaseHardware();
    setError(error, detail.c_str());
    return false;
  }

  UINT32 width = 0;
  UINT32 height = 0;
  hr = MFGetAttributeSize(actualType, MF_MT_FRAME_SIZE, &width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    safeRelease(actualType);
    safeRelease(reader);
    releaseHardware();
    setError(error, "Video has invalid dimensions.");
    return false;
  }

  LONG stride = 0;
  UINT32 strideAttr = 0;
  hr = actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr);
  if (SUCCEEDED(hr)) {
    stride = static_cast<LONG>(strideAttr);
  } else {
    stride = 0;
    if (format == PixelFormat::NV12) {
      stride = static_cast<LONG>(width);
    } else if (format == PixelFormat::P010) {
      stride = static_cast<LONG>(width * 2);
    } else {
      MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                     static_cast<DWORD>(width), &stride);
    }
  }

  Impl* impl = new Impl();
  impl->reader = reader;
  impl->dxgiManager = dxgiManager;
  impl->d3dDevice = d3dDevice;
  impl->d3dContext = d3dContext;
  impl->width = static_cast<int>(width);
  impl->height = static_cast<int>(height);
  impl->topDown = stride >= 0;
  impl->stride = std::abs(stride);
  if (impl->stride == 0) {
    impl->stride = impl->width * 4;
  }
  impl->subtype = subtype;
  updateColorInfo(actualType, &impl->yuvMatrix, &impl->fullRange);
  safeRelease(actualType);

  PROPVARIANT duration{};
  PropVariantInit(&duration);
  hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                        MF_PD_DURATION, &duration);
  if (SUCCEEDED(hr) && duration.vt == VT_UI8) {
    impl->duration100ns = static_cast<int64_t>(duration.uhVal.QuadPart);
  }
  PropVariantClear(&duration);

  impl_ = impl;
  return true;
}

void VideoDecoder::uninit() {
  if (!impl_) return;
  safeRelease(impl_->reader);
  safeRelease(impl_->dxgiManager);
  safeRelease(impl_->d3dContext);
  safeRelease(impl_->d3dDevice);
  delete impl_;
  impl_ = nullptr;
}

bool VideoDecoder::setTargetSize(int targetWidth, int targetHeight,
                                 std::string* error) {
  if (!impl_ || !impl_->reader) {
    setError(error, "Video decoder is not initialized.");
    return false;
  }
  if (targetWidth <= 0 || targetHeight <= 0) {
    setError(error, "Invalid target size for video decoding.");
    return false;
  }
  if (impl_->width == targetWidth && impl_->height == targetHeight) {
    return true;
  }

  IMFMediaType* currentType = nullptr;
  HRESULT hr = impl_->reader->GetCurrentMediaType(
      MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
  if (FAILED(hr) || !currentType) {
    setError(error, ("Failed to query current video format (hr=" +
                     hrToString(hr) + ").")
                        .c_str());
    safeRelease(currentType);
    return false;
  }

  GUID subtype = impl_->subtype;
  currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
  safeRelease(currentType);

  IMFMediaType* targetType = nullptr;
  hr = MFCreateMediaType(&targetType);
  if (FAILED(hr)) {
    setError(error, ("Failed to prepare scaled video format (hr=" +
                     hrToString(hr) + ").")
                        .c_str());
    safeRelease(targetType);
    return false;
  }
  hr = targetType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = targetType->SetGUID(MF_MT_SUBTYPE, subtype);
  if (SUCCEEDED(hr)) {
    hr = MFSetAttributeSize(targetType, MF_MT_FRAME_SIZE,
                            static_cast<UINT32>(targetWidth),
                            static_cast<UINT32>(targetHeight));
  }
  if (FAILED(hr)) {
    setError(error, ("Failed to configure target video size (hr=" +
                     hrToString(hr) + ").")
                        .c_str());
    safeRelease(targetType);
    return false;
  }

  hr = impl_->reader->SetCurrentMediaType(
      MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, targetType);
  safeRelease(targetType);
  if (FAILED(hr)) {
    setError(error, ("Failed to apply target video size (hr=" +
                     hrToString(hr) + ").")
                        .c_str());
    return false;
  }

  IMFMediaType* actualType = nullptr;
  hr = impl_->reader->GetCurrentMediaType(
      MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actualType);
  if (FAILED(hr) || !actualType) {
    setError(error, ("Failed to read scaled video format (hr=" +
                     hrToString(hr) + ").")
                        .c_str());
    safeRelease(actualType);
    return false;
  }

  GUID actualSubtype = GUID_NULL;
  if (FAILED(actualType->GetGUID(MF_MT_SUBTYPE, &actualSubtype))) {
    safeRelease(actualType);
    setError(error, "Failed to read scaled video pixel format.");
    return false;
  }
  PixelFormat format = pixelFormatFromSubtype(actualSubtype);
  if (format == PixelFormat::Unknown) {
    std::string detail =
        "Unsupported scaled video pixel format: " +
        pixelFormatName(actualSubtype);
    safeRelease(actualType);
    setError(error, detail.c_str());
    return false;
  }

  UINT32 width = 0;
  UINT32 height = 0;
  hr = MFGetAttributeSize(actualType, MF_MT_FRAME_SIZE, &width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    safeRelease(actualType);
    setError(error, "Scaled video output has invalid dimensions.");
    return false;
  }

  LONG stride = 0;
  UINT32 strideAttr = 0;
  hr = actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr);
  if (SUCCEEDED(hr)) {
    stride = static_cast<LONG>(strideAttr);
  } else {
    stride = 0;
    if (format == PixelFormat::NV12) {
      stride = static_cast<LONG>(width);
    } else if (format == PixelFormat::P010) {
      stride = static_cast<LONG>(width * 2);
    } else {
      MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                     static_cast<DWORD>(width), &stride);
    }
  }

  impl_->width = static_cast<int>(width);
  impl_->height = static_cast<int>(height);
  impl_->topDown = stride >= 0;
  impl_->stride = std::abs(stride);
  if (impl_->stride == 0) {
    impl_->stride = impl_->width * 4;
  }
  impl_->subtype = actualSubtype;
  updateColorInfo(actualType, &impl_->yuvMatrix, &impl_->fullRange);
  safeRelease(actualType);
  return true;
}

void VideoDecoder::flush() {
  if (!impl_ || !impl_->reader) return;
  impl_->reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  impl_->atEnd = false;
}

bool VideoDecoder::readFrame(VideoFrame& out, VideoReadInfo* info,
                             bool decodePixels) {
  if (!impl_ || impl_->atEnd) return false;

  int streamTicks = 0;
  int typeChanges = 0;
  if (info) {
    *info = VideoReadInfo{};
  }
  uint32_t recoveries = 0;
  uint32_t lastErrorHr = 0;
  constexpr int kMaxRecoveries = 4;
  constexpr int64_t kFallbackDuration100ns = 333667;

  while (true) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    LONGLONG timestamp = 0;
    HRESULT hr = impl_->reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp,
        &sample);
    if (FAILED(hr)) {
      lastErrorHr = static_cast<uint32_t>(hr);
      if (recoveries < static_cast<uint32_t>(kMaxRecoveries)) {
        ++recoveries;
        ++impl_->consecutiveErrors;
        impl_->reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        int64_t lastTs = impl_->lastTimestamp100ns;
        int64_t step = impl_->lastDuration100ns > 0
                           ? impl_->lastDuration100ns
                           : kFallbackDuration100ns;
        if (lastTs > 0 && step <= 0) {
          step = kFallbackDuration100ns;
        }
        if (lastTs > 0) {
          PROPVARIANT pos{};
          PropVariantInit(&pos);
          pos.vt = VT_I8;
          pos.hVal.QuadPart = static_cast<LONGLONG>(lastTs + step);
          impl_->reader->SetCurrentPosition(GUID_NULL, pos);
          PropVariantClear(&pos);
        }
        safeRelease(sample);
        continue;
      }
      impl_->atEnd = true;
      if (info) {
        info->errorHr = lastErrorHr;
        info->recoveries = recoveries;
      }
      safeRelease(sample);
      return false;
    }
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
      ++typeChanges;
      IMFMediaType* newType = nullptr;
      if (SUCCEEDED(impl_->reader->GetCurrentMediaType(
              MF_SOURCE_READER_FIRST_VIDEO_STREAM, &newType)) &&
          newType) {
        UINT32 width = 0;
        UINT32 height = 0;
        HRESULT fmtHr =
            MFGetAttributeSize(newType, MF_MT_FRAME_SIZE, &width, &height);
        if (SUCCEEDED(fmtHr) && width > 0 && height > 0) {
          GUID subtype = GUID_NULL;
          if (SUCCEEDED(newType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            impl_->subtype = subtype;
          }
          LONG stride = 0;
          UINT32 strideAttr = 0;
          fmtHr = newType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr);
          if (SUCCEEDED(fmtHr)) {
            stride = static_cast<LONG>(strideAttr);
          } else {
            stride = 0;
            PixelFormat format = pixelFormatFromSubtype(impl_->subtype);
            if (format == PixelFormat::NV12) {
              stride = static_cast<LONG>(width);
            } else if (format == PixelFormat::P010) {
              stride = static_cast<LONG>(width * 2);
            } else {
              MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                             static_cast<DWORD>(width),
                                             &stride);
            }
          }
          impl_->width = static_cast<int>(width);
          impl_->height = static_cast<int>(height);
          impl_->topDown = stride >= 0;
          impl_->stride = std::abs(stride);
          if (impl_->stride == 0) {
            impl_->stride = impl_->width * 4;
          }
          updateColorInfo(newType, &impl_->yuvMatrix, &impl_->fullRange);
        }
      }
      safeRelease(newType);
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      impl_->atEnd = true;
      impl_->consecutiveErrors = 0;
      if (info) {
        info->errorHr = lastErrorHr;
        info->recoveries = recoveries;
      }
      safeRelease(sample);
      return false;
    }
    if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
      ++streamTicks;
      safeRelease(sample);
      continue;
    }
    if (!sample) {
      continue;
    }

    const int width = impl_->width;
    const int height = impl_->height;
    if (width <= 0 || height <= 0) {
      safeRelease(sample);
      continue;
    }

    if (!decodePixels) {
      out.width = width;
      out.height = height;
      out.timestamp100ns = static_cast<int64_t>(timestamp);
      out.rgba.clear();
      LONGLONG duration = 0;
      if (FAILED(sample->GetSampleDuration(&duration))) {
        duration = 0;
      }
      impl_->lastTimestamp100ns = static_cast<int64_t>(timestamp);
      if (duration > 0) {
        impl_->lastDuration100ns = static_cast<int64_t>(duration);
      }
      impl_->consecutiveErrors = 0;
      if (info) {
        info->timestamp100ns = static_cast<int64_t>(timestamp);
        info->duration100ns = static_cast<int64_t>(duration);
        info->flags = static_cast<uint32_t>(flags);
        info->streamTicks = streamTicks;
        info->typeChanges = typeChanges;
        info->errorHr = lastErrorHr;
        info->recoveries = recoveries;
      }
      safeRelease(sample);
      return true;
    }

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) {
      safeRelease(buffer);
      safeRelease(sample);
      continue;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    LONG pitch = 0;
    bool locked = false;
    bool locked2d = false;
    IMF2DBuffer2* buffer2d = nullptr;
    hr = buffer->QueryInterface(__uuidof(IMF2DBuffer2),
                                reinterpret_cast<void**>(&buffer2d));
    if (SUCCEEDED(hr) && buffer2d) {
      BYTE* scanline0 = nullptr;
      BYTE* bufferStart = nullptr;
      DWORD bufferLength = 0;
      HRESULT hr2d = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0,
                                          &pitch, &bufferStart,
                                          &bufferLength);
      if (SUCCEEDED(hr2d) && scanline0 && bufferLength > 0) {
        data = scanline0;
        curLen = bufferLength;
        locked2d = true;
      } else {
        safeRelease(buffer2d);
      }
    }
    if (!locked2d) {
      hr = buffer->Lock(&data, &maxLen, &curLen);
      if (SUCCEEDED(hr) && data && curLen > 0) {
        locked = true;
      } else {
        safeRelease(buffer);
        safeRelease(sample);
        continue;
      }
    }
    auto unlockBuffer = [&]() {
      if (locked2d && buffer2d) {
        buffer2d->Unlock2D();
      } else if (locked) {
        buffer->Unlock();
      }
      safeRelease(buffer2d);
    };

    constexpr size_t kMaxPixels = 16384u * 16384u;
    size_t pixelCount =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount == 0 || pixelCount > kMaxPixels) {
      unlockBuffer();
      safeRelease(buffer);
      safeRelease(sample);
      continue;
    }
    size_t byteCount = pixelCount * 4u;
    if (byteCount / 4u != pixelCount) {
      unlockBuffer();
      safeRelease(buffer);
      safeRelease(sample);
      continue;
    }

    PixelFormat format = pixelFormatFromSubtype(impl_->subtype);
    if (format == PixelFormat::Unknown) {
      unlockBuffer();
      safeRelease(buffer);
      safeRelease(sample);
      continue;
    }

    int stride = impl_->stride;
    bool topDown = impl_->topDown;
    if (locked2d && pitch > 0) {
      stride = static_cast<int>(pitch);
      topDown = true;
    }

    out.width = width;
    out.height = height;
    out.timestamp100ns = static_cast<int64_t>(timestamp);
    try {
      out.rgba.resize(byteCount);
    } catch (const std::bad_alloc&) {
      impl_->atEnd = true;
      unlockBuffer();
      safeRelease(buffer);
      safeRelease(sample);
      return false;
    }

    if (format == PixelFormat::RGB32 || format == PixelFormat::ARGB32) {
      if (stride < width * 4 || curLen < static_cast<DWORD>(stride)) {
        unlockBuffer();
        safeRelease(buffer);
        safeRelease(sample);
        continue;
      }
      const size_t minBytes = static_cast<size_t>(stride) *
                              static_cast<size_t>(height);
      int maxRows = height;
      if (curLen > 0 && curLen < minBytes) {
        maxRows = std::min(height, static_cast<int>(curLen / stride));
      }

      for (int y = 0; y < maxRows; ++y) {
        int srcY = topDown ? y : (height - 1 - y);
        const uint8_t* srcRow = data + srcY * stride;
        uint8_t* dstRow = out.rgba.data() + (y * width * 4);
        for (int x = 0; x < width; ++x) {
          const uint8_t b = srcRow[x * 4 + 0];
          const uint8_t g = srcRow[x * 4 + 1];
          const uint8_t r = srcRow[x * 4 + 2];
          dstRow[x * 4 + 0] = r;
          dstRow[x * 4 + 1] = g;
          dstRow[x * 4 + 2] = b;
          dstRow[x * 4 + 3] = 255;
        }
      }
      for (int y = maxRows; y < height; ++y) {
        uint8_t* dstRow = out.rgba.data() + (y * width * 4);
        std::fill(dstRow, dstRow + width * 4, 0);
      }
    } else {
      size_t minStride = (format == PixelFormat::P010)
                             ? static_cast<size_t>(width) * 2u
                             : static_cast<size_t>(width);
      if (stride < static_cast<int>(minStride)) {
        unlockBuffer();
        safeRelease(buffer);
        safeRelease(sample);
        continue;
      }
      size_t required =
          static_cast<size_t>(stride) * static_cast<size_t>(height) * 3u / 2u;
      if (curLen < required) {
        unlockBuffer();
        safeRelease(buffer);
        safeRelease(sample);
        continue;
      }

      const uint8_t* yPlane = data;
      const uint8_t* uvPlane =
          data + static_cast<size_t>(stride) * static_cast<size_t>(height);

      for (int y = 0; y < height; ++y) {
        int srcY = topDown ? y : (height - 1 - y);
        int uvY = srcY / 2;
        uint8_t* dstRow = out.rgba.data() + (y * width * 4);

        if (format == PixelFormat::NV12) {
          const uint8_t* yRow = yPlane + srcY * stride;
          const uint8_t* uvRow = uvPlane + uvY * stride;
          for (int x = 0; x < width; ++x) {
            int yVal = yRow[x];
            int uvIndex = (x / 2) * 2;
            int uVal = uvRow[uvIndex + 0];
            int vVal = uvRow[uvIndex + 1];
            uint8_t r = 0, g = 0, b = 0;
            yuvToRgb(yVal, uVal, vVal, impl_->fullRange, impl_->yuvMatrix, r,
                     g, b);
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = 255;
          }
        } else {
          const uint16_t* yRow =
              reinterpret_cast<const uint16_t*>(yPlane + srcY * stride);
          const uint16_t* uvRow =
              reinterpret_cast<const uint16_t*>(uvPlane + uvY * stride);
          for (int x = 0; x < width; ++x) {
            int uvIndex = (x / 2) * 2;
            int y10 = static_cast<int>(yRow[x] >> 6);
            int u10 = static_cast<int>(uvRow[uvIndex + 0] >> 6);
            int v10 = static_cast<int>(uvRow[uvIndex + 1] >> 6);
            int y8 = (y10 * 255 + 511) / 1023;
            int u8 = (u10 * 255 + 511) / 1023;
            int v8 = (v10 * 255 + 511) / 1023;
            uint8_t r = 0, g = 0, b = 0;
            yuvToRgb(y8, u8, v8, impl_->fullRange, impl_->yuvMatrix, r, g, b);
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = 255;
          }
        }
      }
    }

    LONGLONG duration = 0;
    if (FAILED(sample->GetSampleDuration(&duration))) {
      duration = 0;
    }
    impl_->lastTimestamp100ns = static_cast<int64_t>(timestamp);
    if (duration > 0) {
      impl_->lastDuration100ns = static_cast<int64_t>(duration);
    }
    impl_->consecutiveErrors = 0;
    if (info) {
      info->timestamp100ns = static_cast<int64_t>(timestamp);
      info->duration100ns = static_cast<int64_t>(duration);
      info->flags = static_cast<uint32_t>(flags);
      info->streamTicks = streamTicks;
      info->typeChanges = typeChanges;
      info->errorHr = lastErrorHr;
      info->recoveries = recoveries;
    }

    unlockBuffer();
    safeRelease(buffer);
    safeRelease(sample);
    return true;
  }
}

bool VideoDecoder::seekToTimestamp100ns(int64_t timestamp100ns) {
  if (!impl_ || !impl_->reader) return false;
  if (timestamp100ns < 0) timestamp100ns = 0;
  PROPVARIANT pos{};
  PropVariantInit(&pos);
  pos.vt = VT_I8;
  pos.hVal.QuadPart = static_cast<LONGLONG>(timestamp100ns);
  HRESULT hr = impl_->reader->SetCurrentPosition(GUID_NULL, pos);
  PropVariantClear(&pos);
  if (FAILED(hr)) return false;
  impl_->atEnd = false;
  return true;
}

bool VideoDecoder::atEnd() const { return impl_ ? impl_->atEnd : true; }

int VideoDecoder::width() const { return impl_ ? impl_->width : 0; }
int VideoDecoder::height() const { return impl_ ? impl_->height : 0; }

int64_t VideoDecoder::duration100ns() const {
  return impl_ ? impl_->duration100ns : 0;
}
