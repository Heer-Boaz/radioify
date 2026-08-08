#include "playback/video/image_wic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>
#include <new>

namespace playback_video_image {
namespace {

using Microsoft::WRL::ComPtr;

void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

void clearError(std::string* error) {
  if (error) error->clear();
}

bool decodeFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                 RgbaImage* image, const DecodeLimits& limits,
                 std::string* error) {
  if (!factory || !decoder || !image) return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    setError(error, "Failed to decode the image frame.");
    return false;
  }

  UINT width = 0;
  UINT height = 0;
  hr = frame->GetSize(&width, &height);
  if (FAILED(hr) || width == 0 || height == 0 ||
      width > limits.maxWidth || height > limits.maxHeight ||
      width > (std::numeric_limits<UINT>::max)() / 4u) {
    setError(error, "The decoded image dimensions exceed the allowed limits.");
    return false;
  }
  const UINT stride = width * 4u;
  size_t decodedBytes = 0;
  if (!requiredBytes(width, height, stride, &decodedBytes) ||
      decodedBytes > limits.maxDecodedBytes ||
      decodedBytes > (std::numeric_limits<UINT>::max)()) {
    setError(error, "The decoded image size exceeds the allowed limits.");
    return false;
  }

  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
  }
  if (FAILED(hr)) {
    setError(error, "Failed to convert the decoded image to RGBA.");
    return false;
  }

  try {
    image->pixels.assign(decodedBytes, 0u);
  } catch (const std::bad_alloc&) {
    setError(error, "Failed to allocate the decoded image.");
    return false;
  }
  hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(decodedBytes),
                             image->pixels.data());
  if (FAILED(hr)) {
    *image = RgbaImage{};
    setError(error, "Failed to read the decoded image pixels.");
    return false;
  }
  image->width = width;
  image->height = height;
  image->strideBytes = stride;
  return true;
}

}  // namespace

struct WicCodec::Impl {
  ComPtr<IWICImagingFactory> factory;
  HRESULT comResult = E_FAIL;
  DWORD ownerThreadId = 0;

  bool ownsComInitialization() const { return SUCCEEDED(comResult); }
};

WicCodec::WicCodec() : impl_(std::make_unique<Impl>()) {}

WicCodec::~WicCodec() { close(); }

bool WicCodec::open(std::string* error) {
  clearError(error);
  close();
  impl_->ownerThreadId = GetCurrentThreadId();
  impl_->comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(impl_->comResult) && impl_->comResult != RPC_E_CHANGED_MODE) {
    setError(error, "Failed to initialize COM for the image codec.");
    impl_->ownerThreadId = 0;
    return false;
  }
  const HRESULT factoryResult =
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&impl_->factory));
  if (FAILED(factoryResult)) {
    setError(error, "Failed to create the Windows image codec.");
    close();
    return false;
  }
  return true;
}

void WicCodec::close() {
  if (!impl_) return;
  impl_->factory.Reset();
  if (impl_->ownsComInitialization() &&
      impl_->ownerThreadId == GetCurrentThreadId()) {
    CoUninitialize();
  }
  impl_->comResult = E_FAIL;
  impl_->ownerThreadId = 0;
}

bool WicCodec::isOpen() const {
  return impl_ && impl_->factory != nullptr &&
         impl_->ownerThreadId == GetCurrentThreadId();
}

bool WicCodec::decodeFile(const std::filesystem::path& path, RgbaImage* image,
                          const DecodeLimits& limits, std::string* error) {
  clearError(error);
  if (!image) {
    setError(error, "No decoded image destination was provided.");
    return false;
  }
  *image = RgbaImage{};
  if (!isOpen() || path.empty()) {
    setError(error, "The Windows image codec is not available.");
    return false;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = impl_->factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      &decoder);
  if (FAILED(hr)) {
    setError(error, "Failed to open the image.");
    return false;
  }
  return decodeFrame(impl_->factory.Get(), decoder.Get(), image, limits,
                     error);
}

bool WicCodec::decodeBytes(const uint8_t* bytes, size_t size, RgbaImage* image,
                           const DecodeLimits& limits, std::string* error) {
  clearError(error);
  if (!image) {
    setError(error, "No decoded image destination was provided.");
    return false;
  }
  *image = RgbaImage{};
  if (!isOpen() || !bytes || size == 0 ||
      size > (std::numeric_limits<DWORD>::max)()) {
    setError(error, "The encoded image bytes are invalid.");
    return false;
  }
  ComPtr<IWICStream> stream;
  HRESULT hr = impl_->factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) {
    hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes)),
        static_cast<DWORD>(size));
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (SUCCEEDED(hr)) {
    hr = impl_->factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
  }
  if (FAILED(hr)) {
    setError(error, "Failed to open the encoded image.");
    return false;
  }
  return decodeFrame(impl_->factory.Get(), decoder.Get(), image, limits,
                     error);
}

bool WicCodec::encodePng(const RgbaImageView& image,
                         std::vector<uint8_t>* png, std::string* error) {
  clearError(error);
  if (!png) {
    setError(error, "No encoded image destination was provided.");
    return false;
  }
  png->clear();
  if (!isOpen() || !validate(image) ||
      image.width > (std::numeric_limits<UINT>::max)() / 4u) {
    setError(error, "The RGBA image is invalid or the codec is unavailable.");
    return false;
  }

  const UINT tightStride = image.width * 4u;
  const uint64_t tightSize64 =
      static_cast<uint64_t>(tightStride) * image.height;
  if (tightSize64 == 0 ||
      tightSize64 > (std::numeric_limits<UINT>::max)() ||
      tightSize64 > (std::numeric_limits<size_t>::max)()) {
    setError(error, "The PNG input image is too large.");
    return false;
  }
  const size_t tightSize = static_cast<size_t>(tightSize64);
  std::vector<uint8_t> bgra;
  try {
    bgra.resize(tightSize);
  } catch (const std::bad_alloc&) {
    setError(error, "Failed to allocate the PNG input image.");
    return false;
  }
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* source =
        image.pixels + static_cast<size_t>(y) * image.strideBytes;
    uint8_t* destination =
        bgra.data() + static_cast<size_t>(y) * tightStride;
    for (uint32_t x = 0; x < image.width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4u;
      destination[offset + 0] = source[offset + 2];
      destination[offset + 1] = source[offset + 1];
      destination[offset + 2] = source[offset + 0];
      destination[offset + 3] = source[offset + 3];
    }
  }

  ComPtr<IStream> stream;
  HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr)) {
    hr = impl_->factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                       &encoder);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
  if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
  if (SUCCEEDED(hr)) hr = frame->SetSize(image.width, image.height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr) &&
      !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
    hr = E_FAIL;
  }
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(image.height, tightStride,
                            static_cast<UINT>(tightSize), bgra.data());
  }
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) {
    setError(error, "Failed to encode the RGBA image as PNG.");
    return false;
  }

  STATSTG stats{};
  if (FAILED(stream->Stat(&stats, STATFLAG_NONAME)) ||
      stats.cbSize.QuadPart <= 0 ||
      static_cast<ULONGLONG>(stats.cbSize.QuadPart) >
          (std::numeric_limits<size_t>::max)()) {
    setError(error, "The encoded PNG has an invalid size.");
    return false;
  }
  HGLOBAL encodedMemory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &encodedMemory)) ||
      !encodedMemory) {
    setError(error, "Failed to access the encoded PNG.");
    return false;
  }
  const void* encodedBytes = GlobalLock(encodedMemory);
  if (!encodedBytes) {
    setError(error, "Failed to lock the encoded PNG.");
    return false;
  }
  const size_t encodedSize = static_cast<size_t>(stats.cbSize.QuadPart);
  try {
    png->assign(static_cast<const uint8_t*>(encodedBytes),
                static_cast<const uint8_t*>(encodedBytes) + encodedSize);
  } catch (const std::bad_alloc&) {
    GlobalUnlock(encodedMemory);
    setError(error, "Failed to allocate the encoded PNG.");
    return false;
  }
  GlobalUnlock(encodedMemory);
  return true;
}

}  // namespace playback_video_image
