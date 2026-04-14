#include "asciiart_image_decode_wic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <windows.h>

#include <limits>

namespace {

void setError(std::string* error, const char* message) {
  if (error) {
    *error = message;
  }
}

template <typename T>
void safeRelease(T*& ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

class ComScope {
 public:
  explicit ComScope(std::string* error) {
    hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
      setError(error, "Failed to initialize COM for image decoding.");
    }
  }

  ~ComScope() {
    if (SUCCEEDED(hr_)) {
      CoUninitialize();
    }
  }

  bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_{E_FAIL};
};

bool copyDecoderPixels(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                       int& outWidth, int& outHeight,
                       std::vector<uint8_t>& outPixels, std::string* error) {
  if (!factory || !decoder) {
    setError(error, "Failed to open image.");
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  HRESULT hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    setError(error, "Failed to decode image frame.");
    return false;
  }

  UINT width = 0;
  UINT height = 0;
  frame->GetSize(&width, &height);
  if (width == 0 || height == 0) {
    safeRelease(frame);
    setError(error, "Image has invalid dimensions.");
    return false;
  }
  outWidth = static_cast<int>(width);
  outHeight = static_cast<int>(height);

  IWICFormatConverter* converter = nullptr;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    safeRelease(frame);
    setError(error, "Failed to convert image.");
    return false;
  }

  hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    safeRelease(converter);
    safeRelease(frame);
    setError(error, "Failed to convert image format.");
    return false;
  }

  const UINT stride = width * 4;
  const UINT bufferSize = stride * height;
  outPixels.assign(bufferSize, 0);
  hr = converter->CopyPixels(nullptr, stride, bufferSize, outPixels.data());
  if (FAILED(hr)) {
    safeRelease(converter);
    safeRelease(frame);
    setError(error, "Failed to read image pixels.");
    return false;
  }

  safeRelease(converter);
  safeRelease(frame);
  return true;
}

}  // namespace

bool decodeImageFileToRgba(const std::filesystem::path& path, int& outWidth,
                           int& outHeight, std::vector<uint8_t>& outPixels,
                           std::string* error) {
  ComScope com(error);
  if (!com.ok()) {
    return false;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    setError(error, "Failed to create image decoder.");
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  const std::wstring widePath = path.wstring();
  hr = factory->CreateDecoderFromFilename(
      widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      &decoder);
  if (FAILED(hr)) {
    safeRelease(factory);
    setError(error, "Failed to open image.");
    return false;
  }

  const bool ok =
      copyDecoderPixels(factory, decoder, outWidth, outHeight, outPixels, error);
  safeRelease(decoder);
  safeRelease(factory);
  return ok;
}

bool decodeImageBytesToRgba(const uint8_t* bytes, size_t size, int& outWidth,
                            int& outHeight, std::vector<uint8_t>& outPixels,
                            std::string* error) {
  if (!bytes || size == 0 ||
      size > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
    setError(error, "Failed to open image.");
    return false;
  }

  ComScope com(error);
  if (!com.ok()) {
    return false;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    setError(error, "Failed to create image decoder.");
    return false;
  }

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr)) {
    safeRelease(factory);
    setError(error, "Failed to create image stream.");
    return false;
  }

  hr = stream->InitializeFromMemory(
      const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes)),
      static_cast<DWORD>(size));
  if (FAILED(hr)) {
    safeRelease(stream);
    safeRelease(factory);
    setError(error, "Failed to open image.");
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnDemand,
                                        &decoder);
  if (FAILED(hr)) {
    safeRelease(stream);
    safeRelease(factory);
    setError(error, "Failed to open image.");
    return false;
  }

  const bool ok =
      copyDecoderPixels(factory, decoder, outWidth, outHeight, outPixels, error);
  safeRelease(decoder);
  safeRelease(stream);
  safeRelease(factory);
  return ok;
}
