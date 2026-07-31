#include "frame_clipboard.h"

#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

namespace playback_video_frame_clipboard {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kClipboardOpenAttempts = 6;
constexpr auto kClipboardOpenRetryDelay = std::chrono::milliseconds(5);

void setError(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
}

void clearError(std::string* error) {
  if (error) {
    error->clear();
  }
}

std::string windowsError(const char* operation) {
  return std::string(operation) + " failed (Windows error " +
         std::to_string(GetLastError()) + ").";
}

bool validateSnapshot(const VideoFrameSnapshot& snapshot,
                      size_t* rowBytes, size_t* pixelBytes,
                      std::string* error) {
  if (snapshot.width == 0 || snapshot.height == 0) {
    setError(error, "The rendered frame has invalid dimensions.");
    return false;
  }
  if (snapshot.width >
          static_cast<uint32_t>(std::numeric_limits<LONG>::max()) ||
      snapshot.height >
          static_cast<uint32_t>(std::numeric_limits<LONG>::max())) {
    setError(error, "The rendered frame is too large for the Windows clipboard.");
    return false;
  }

  const size_t width = static_cast<size_t>(snapshot.width);
  const size_t height = static_cast<size_t>(snapshot.height);
  if (width > std::numeric_limits<size_t>::max() / 4u) {
    setError(error, "The rendered frame row size overflows.");
    return false;
  }
  const size_t resolvedRowBytes = width * 4u;
  if (snapshot.strideBytes < resolvedRowBytes) {
    setError(error, "The rendered frame stride is smaller than one RGBA row.");
    return false;
  }
  if (height > std::numeric_limits<size_t>::max() / resolvedRowBytes) {
    setError(error, "The rendered frame pixel size overflows.");
    return false;
  }
  const size_t resolvedPixelBytes = resolvedRowBytes * height;
  const size_t strideBytes = static_cast<size_t>(snapshot.strideBytes);
  if (height - 1u > std::numeric_limits<size_t>::max() / strideBytes) {
    setError(error, "The rendered frame stride range overflows.");
    return false;
  }
  const size_t lastRowOffset = strideBytes * (height - 1u);
  if (lastRowOffset > snapshot.rgba.size() ||
      resolvedRowBytes > snapshot.rgba.size() - lastRowOffset) {
    setError(error, "The rendered frame does not contain all RGBA rows.");
    return false;
  }
  if (resolvedPixelBytes > std::numeric_limits<DWORD>::max()) {
    setError(error, "The rendered frame exceeds the Windows DIB size limit.");
    return false;
  }

  *rowBytes = resolvedRowBytes;
  *pixelBytes = resolvedPixelBytes;
  return true;
}

class GlobalMemory {
 public:
  GlobalMemory() = default;
  explicit GlobalMemory(HGLOBAL memory) : memory_(memory) {}
  ~GlobalMemory() {
    if (memory_) {
      GlobalFree(memory_);
    }
  }

  GlobalMemory(const GlobalMemory&) = delete;
  GlobalMemory& operator=(const GlobalMemory&) = delete;

  GlobalMemory(GlobalMemory&& other) noexcept : memory_(other.release()) {}
  GlobalMemory& operator=(GlobalMemory&& other) noexcept {
    if (this != &other) {
      if (memory_) {
        GlobalFree(memory_);
      }
      memory_ = other.release();
    }
    return *this;
  }

  HGLOBAL get() const { return memory_; }

  HGLOBAL release() {
    HGLOBAL memory = memory_;
    memory_ = nullptr;
    return memory;
  }

 private:
  HGLOBAL memory_ = nullptr;
};

GlobalMemory allocateClipboardPayload(const uint8_t* bytes, size_t size,
                                      std::string* error) {
  if (!bytes || size == 0) {
    setError(error, "The clipboard image payload is empty.");
    return {};
  }
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!memory) {
    setError(error, windowsError("GlobalAlloc"));
    return {};
  }
  GlobalMemory owned(memory);
  void* destination = GlobalLock(memory);
  if (!destination) {
    setError(error, windowsError("GlobalLock"));
    return {};
  }
  std::memcpy(destination, bytes, size);
  GlobalUnlock(memory);
  return owned;
}

class ComScope {
 public:
  ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

  ~ComScope() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  bool available() const {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_ = E_FAIL;
};

bool encodePng(const uint8_t* bgra, uint32_t width, uint32_t height,
               uint32_t strideBytes, size_t pixelBytes,
               std::vector<uint8_t>* png) {
  if (!bgra || !png || pixelBytes > std::numeric_limits<UINT>::max()) {
    return false;
  }

  ComScope com;
  if (!com.available()) {
    return false;
  }

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    return false;
  }

  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  HRESULT hr =
      factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(&frame, &properties);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Initialize(properties.Get());
  }
  if (SUCCEEDED(hr)) {
    hr = frame->SetSize(width, height);
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(hr)) {
    hr = frame->SetPixelFormat(&format);
  }
  if (SUCCEEDED(hr) &&
      !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
    hr = E_FAIL;
  }
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(height, strideBytes,
                            static_cast<UINT>(pixelBytes),
                            const_cast<BYTE*>(bgra));
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }
  if (FAILED(hr)) {
    return false;
  }

  STATSTG stats{};
  if (FAILED(stream->Stat(&stats, STATFLAG_NONAME)) ||
      stats.cbSize.QuadPart <= 0 ||
      static_cast<ULONGLONG>(stats.cbSize.QuadPart) >
          std::numeric_limits<size_t>::max()) {
    return false;
  }
  HGLOBAL encodedMemory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &encodedMemory)) ||
      !encodedMemory) {
    return false;
  }
  const void* encodedBytes = GlobalLock(encodedMemory);
  if (!encodedBytes) {
    return false;
  }
  const size_t encodedSize = static_cast<size_t>(stats.cbSize.QuadPart);
  png->assign(static_cast<const uint8_t*>(encodedBytes),
              static_cast<const uint8_t*>(encodedBytes) + encodedSize);
  GlobalUnlock(encodedMemory);
  return true;
}

class ClipboardScope {
 public:
  explicit ClipboardScope(HWND owner) {
    for (int attempt = 0; attempt < kClipboardOpenAttempts; ++attempt) {
      if (OpenClipboard(owner)) {
        opened_ = true;
        break;
      }
      if (attempt + 1 < kClipboardOpenAttempts) {
        std::this_thread::sleep_for(kClipboardOpenRetryDelay);
      }
    }
  }

  ~ClipboardScope() {
    if (opened_) {
      CloseClipboard();
    }
  }

  bool opened() const { return opened_; }

 private:
  bool opened_ = false;
};

}  // namespace

bool buildDibV5Payload(const VideoFrameSnapshot& snapshot,
                       std::vector<uint8_t>* payload, std::string* error) {
  clearError(error);
  if (!payload) {
    setError(error, "No DIB payload destination was provided.");
    return false;
  }
  payload->clear();

  size_t rowBytes = 0;
  size_t pixelBytes = 0;
  if (!validateSnapshot(snapshot, &rowBytes, &pixelBytes, error)) {
    return false;
  }
  if (pixelBytes >
      std::numeric_limits<size_t>::max() - sizeof(BITMAPV5HEADER)) {
    setError(error, "The Windows DIB payload size overflows.");
    return false;
  }

  payload->assign(sizeof(BITMAPV5HEADER) + pixelBytes, 0u);
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(BITMAPV5HEADER);
  header.bV5Width = static_cast<LONG>(snapshot.width);
  header.bV5Height = -static_cast<LONG>(snapshot.height);
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5SizeImage = static_cast<DWORD>(pixelBytes);
  header.bV5RedMask = 0x00ff0000u;
  header.bV5GreenMask = 0x0000ff00u;
  header.bV5BlueMask = 0x000000ffu;
  header.bV5AlphaMask = 0xff000000u;
  header.bV5CSType = LCS_sRGB;
  header.bV5Intent = LCS_GM_IMAGES;
  std::memcpy(payload->data(), &header, sizeof(header));

  uint8_t* destination = payload->data() + sizeof(BITMAPV5HEADER);
  for (uint32_t y = 0; y < snapshot.height; ++y) {
    const uint8_t* source =
        snapshot.rgba.data() +
        static_cast<size_t>(snapshot.strideBytes) * static_cast<size_t>(y);
    uint8_t* row =
        destination + rowBytes * static_cast<size_t>(y);
    for (uint32_t x = 0; x < snapshot.width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4u;
      row[offset + 0] = source[offset + 2];
      row[offset + 1] = source[offset + 1];
      row[offset + 2] = source[offset + 0];
      row[offset + 3] = source[offset + 3];
    }
  }
  return true;
}

bool copyToClipboard(HWND owner, const VideoFrameSnapshot& snapshot,
                     std::string* error) {
  clearError(error);
  if (!owner || !IsWindow(owner)) {
    setError(error, "The video window is not available as clipboard owner.");
    return false;
  }

  std::vector<uint8_t> dib;
  if (!buildDibV5Payload(snapshot, &dib, error)) {
    return false;
  }
  GlobalMemory dibMemory =
      allocateClipboardPayload(dib.data(), dib.size(), error);
  if (!dibMemory.get()) {
    return false;
  }

  const uint8_t* bgra = dib.data() + sizeof(BITMAPV5HEADER);
  const size_t pixelBytes = dib.size() - sizeof(BITMAPV5HEADER);
  std::vector<uint8_t> png;
  GlobalMemory pngMemory;
  if (encodePng(bgra, snapshot.width, snapshot.height,
                snapshot.width * 4u, pixelBytes, &png)) {
    pngMemory = allocateClipboardPayload(png.data(), png.size(), nullptr);
  }

  ClipboardScope clipboard(owner);
  if (!clipboard.opened()) {
    setError(error, windowsError("OpenClipboard"));
    return false;
  }
  if (!EmptyClipboard()) {
    setError(error, windowsError("EmptyClipboard"));
    return false;
  }
  if (!SetClipboardData(CF_DIBV5, dibMemory.get())) {
    setError(error, windowsError("SetClipboardData(CF_DIBV5)"));
    return false;
  }
  dibMemory.release();

  if (pngMemory.get()) {
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (pngFormat != 0 &&
        SetClipboardData(pngFormat, pngMemory.get())) {
      pngMemory.release();
    }
  }
  return true;
}

}  // namespace playback_video_frame_clipboard
