#include "videodecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstring>

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
  int width = 0;
  int height = 0;
  int stride = 0;
  bool topDown = true;
  bool atEnd = false;
  int64_t duration100ns = 0;
};

VideoDecoder::~VideoDecoder() { uninit(); }

bool VideoDecoder::init(const std::filesystem::path& path, std::string* error) {
  uninit();

  if (!ensureMediaFoundation(error)) return false;

  IMFAttributes* attributes = nullptr;
  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) {
    hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  }

  IMFSourceReader* reader = nullptr;
  std::wstring wpath = path.wstring();
  hr = MFCreateSourceReaderFromURL(wpath.c_str(), attributes, &reader);
  safeRelease(attributes);
  if (FAILED(hr)) {
    setError(error, "Failed to open video file.");
    return false;
  }

  hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  if (SUCCEEDED(hr)) {
    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  }
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "No video stream found.");
    return false;
  }

  IMFMediaType* nativeType = nullptr;
  hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                  &nativeType);
  safeRelease(nativeType);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "No video stream found.");
    return false;
  }

  IMFMediaType* type = nullptr;
  hr = MFCreateMediaType(&type);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to configure video decoding.");
    return false;
  }

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (SUCCEEDED(hr)) {
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                     nullptr, type);
  }
  safeRelease(type);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to configure video output.");
    return false;
  }

  IMFMediaType* actualType = nullptr;
  hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   &actualType);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to query video format.");
    return false;
  }

  UINT32 width = 0;
  UINT32 height = 0;
  hr = MFGetAttributeSize(actualType, MF_MT_FRAME_SIZE, &width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    safeRelease(actualType);
    safeRelease(reader);
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
    MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                   static_cast<DWORD>(width), &stride);
  }

  safeRelease(actualType);

  Impl* impl = new Impl();
  impl->reader = reader;
  impl->width = static_cast<int>(width);
  impl->height = static_cast<int>(height);
  impl->topDown = stride >= 0;
  impl->stride = std::abs(stride);
  if (impl->stride == 0) {
    impl->stride = impl->width * 4;
  }

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
  delete impl_;
  impl_ = nullptr;
}

bool VideoDecoder::readFrame(VideoFrame& out) {
  if (!impl_ || impl_->atEnd) return false;

  while (true) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    LONGLONG timestamp = 0;
    HRESULT hr = impl_->reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp,
        &sample);
    if (FAILED(hr)) {
      impl_->atEnd = true;
      safeRelease(sample);
      return false;
    }
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
      IMFMediaType* newType = nullptr;
      if (SUCCEEDED(impl_->reader->GetCurrentMediaType(
              MF_SOURCE_READER_FIRST_VIDEO_STREAM, &newType)) &&
          newType) {
        UINT32 width = 0;
        UINT32 height = 0;
        HRESULT fmtHr =
            MFGetAttributeSize(newType, MF_MT_FRAME_SIZE, &width, &height);
        if (SUCCEEDED(fmtHr) && width > 0 && height > 0) {
          LONG stride = 0;
          UINT32 strideAttr = 0;
          fmtHr = newType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr);
          if (SUCCEEDED(fmtHr)) {
            stride = static_cast<LONG>(strideAttr);
          } else {
            stride = 0;
            MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                           static_cast<DWORD>(width), &stride);
          }
          impl_->width = static_cast<int>(width);
          impl_->height = static_cast<int>(height);
          impl_->topDown = stride >= 0;
          impl_->stride = std::abs(stride);
          if (impl_->stride == 0) {
            impl_->stride = impl_->width * 4;
          }
        }
      }
      safeRelease(newType);
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      impl_->atEnd = true;
      safeRelease(sample);
      return false;
    }
    if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
      safeRelease(sample);
      continue;
    }
    if (!sample) {
      continue;
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
    bool locked = false;
    hr = buffer->Lock(&data, &maxLen, &curLen);
    if (SUCCEEDED(hr) && data && curLen > 0) {
      locked = true;
    } else {
      safeRelease(buffer);
      safeRelease(sample);
      continue;
    }

    const int width = impl_->width;
    const int height = impl_->height;
    out.width = width;
    out.height = height;
    out.timestamp100ns = static_cast<int64_t>(timestamp);
    out.rgba.resize(static_cast<size_t>(width * height * 4));

    const size_t minBytes = static_cast<size_t>(impl_->stride) *
                            static_cast<size_t>(height);
    int maxRows = height;
    if (curLen > 0 && curLen < minBytes) {
      maxRows = std::min(height,
                         static_cast<int>(curLen / impl_->stride));
    }

    for (int y = 0; y < maxRows; ++y) {
      int srcY = impl_->topDown ? y : (height - 1 - y);
      const uint8_t* srcRow = data + srcY * impl_->stride;
      uint8_t* dstRow = out.rgba.data() + (y * width * 4);
      for (int x = 0; x < width; ++x) {
        const uint8_t b = srcRow[x * 4 + 0];
        const uint8_t g = srcRow[x * 4 + 1];
        const uint8_t r = srcRow[x * 4 + 2];
        const uint8_t a = srcRow[x * 4 + 3];
        dstRow[x * 4 + 0] = r;
        dstRow[x * 4 + 1] = g;
        dstRow[x * 4 + 2] = b;
        dstRow[x * 4 + 3] = a;
      }
    }
    for (int y = maxRows; y < height; ++y) {
      uint8_t* dstRow = out.rgba.data() + (y * width * 4);
      std::fill(dstRow, dstRow + width * 4, 0);
    }

    if (locked) {
      buffer->Unlock();
    }
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
