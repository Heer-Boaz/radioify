#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include "radioify_explorer_config.h"

namespace {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_serverLockCount = 0;

constexpr AVRational kAvTimeBaseQ{1, AV_TIME_BASE};

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  explicit ComPtr(T* ptr) : ptr_(ptr) {}
  ~ComPtr() { reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T* get() const { return ptr_; }
  T** put() {
    reset();
    return &ptr_;
  }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

  void reset(T* ptr = nullptr) {
    if (ptr_) {
      ptr_->Release();
    }
    ptr_ = ptr;
  }

 private:
  T* ptr_ = nullptr;
};

struct AvFormatInput {
  ~AvFormatInput() {
    if (ptr) {
      avformat_close_input(&ptr);
    }
  }

  AVFormatContext** put() { return &ptr; }
  AVFormatContext* get() const { return ptr; }
  AVFormatContext* operator->() const { return ptr; }

  AVFormatContext* ptr = nullptr;
};

struct AvCodecContextHandle {
  ~AvCodecContextHandle() {
    if (ptr) {
      avcodec_free_context(&ptr);
    }
  }

  AVCodecContext** put() { return &ptr; }
  AVCodecContext* get() const { return ptr; }
  AVCodecContext* operator->() const { return ptr; }

  AVCodecContext* ptr = nullptr;
};

struct AvFrameHandle {
  AvFrameHandle() : ptr(av_frame_alloc()) {}
  ~AvFrameHandle() {
    if (ptr) {
      av_frame_free(&ptr);
    }
  }

  AVFrame* get() const { return ptr; }
  AVFrame* operator->() const { return ptr; }

  AVFrame* ptr = nullptr;
};

struct AvPacketHandle {
  AvPacketHandle() : ptr(av_packet_alloc()) {}
  ~AvPacketHandle() {
    if (ptr) {
      av_packet_free(&ptr);
    }
  }

  AVPacket* get() const { return ptr; }
  AVPacket* operator->() const { return ptr; }

  AVPacket* ptr = nullptr;
};

struct SwsContextHandle {
  ~SwsContextHandle() {
    if (ptr) {
      sws_freeContext(ptr);
    }
  }

  SwsContext* ptr = nullptr;
};

struct WinHandle {
  ~WinHandle() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
};

HRESULT duplicateString(std::wstring_view text, LPWSTR* out) noexcept {
  if (!out) return E_POINTER;
  *out = nullptr;

  const size_t length = text.size() + 1;
  wchar_t* buffer =
      static_cast<wchar_t*>(CoTaskMemAlloc(length * sizeof(wchar_t)));
  if (!buffer) return E_OUTOFMEMORY;

  const HRESULT hr = StringCchCopyNW(buffer, length, text.data(), text.size());
  if (FAILED(hr)) {
    CoTaskMemFree(buffer);
    return hr;
  }

  *out = buffer;
  return S_OK;
}

const CLSID& radioifyExplorerCommandClsid() noexcept {
  static const CLSID clsid = [] {
    CLSID parsed{};
    const HRESULT hr = CLSIDFromString(
        const_cast<LPWSTR>(RADIOIFY_WIN11_EXPLORER_COMMAND_CLSID_W), &parsed);
    return SUCCEEDED(hr) ? parsed : CLSID{};
  }();
  return clsid;
}

const CLSID& radioifyExplorerThumbnailClsid() noexcept {
  static const CLSID clsid = [] {
    CLSID parsed{};
    const HRESULT hr = CLSIDFromString(
        const_cast<LPWSTR>(RADIOIFY_WIN11_EXPLORER_THUMBNAIL_CLSID_W), &parsed);
    return SUCCEEDED(hr) ? parsed : CLSID{};
  }();
  return clsid;
}

std::wstring getModulePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(g_module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    if (buffer.size() >= 32768) return {};
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring parentPathOf(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring_view::npos) return {};
  return std::wstring(path.substr(0, slash));
}

std::wstring joinPath(std::wstring_view parent, std::wstring_view child) {
  if (parent.empty()) return std::wstring(child);
  std::wstring result(parent);
  if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
  result.append(child);
  return result;
}

bool isExistingFile(const std::wstring& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

HRESULT widePathToUtf8(const std::wstring& path, std::string* out) {
  if (!out) return E_POINTER;
  out->clear();
  if (path.empty()) return E_INVALIDARG;

  const int needed = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()),
      nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return HRESULT_FROM_WIN32(GetLastError());

  out->resize(static_cast<size_t>(needed));
  const int written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()),
      out->data(), needed, nullptr, nullptr);
  if (written <= 0) {
    out->clear();
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

std::wstring lowercasePathText(std::wstring value) {
  for (wchar_t& ch : value) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return value;
}

std::wstring extensionOfPath(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring_view::npos ||
      (slash != std::wstring_view::npos && dot < slash)) {
    return {};
  }
  return lowercasePathText(std::wstring(path.substr(dot)));
}

std::wstring filenameStemOfPath(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t begin = slash == std::wstring_view::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of(L'.');
  const size_t end =
      (dot == std::wstring_view::npos || dot < begin) ? path.size() : dot;
  return std::wstring(path.substr(begin, end - begin));
}

bool isOneOfExtension(std::wstring_view ext,
                      const std::array<std::wstring_view, 4>& values) {
  for (std::wstring_view value : values) {
    if (ext == value) return true;
  }
  return false;
}

bool isSupportedImageExtension(std::wstring_view ext) {
  static constexpr std::array<std::wstring_view, 4> kImageExts = {
      L".jpg", L".jpeg", L".png", L".bmp"};
  return isOneOfExtension(ext, kImageExts);
}

bool isSupportedVideoExtension(std::wstring_view ext) {
  static constexpr std::array<std::wstring_view, 22> kVideoExts = {
      L".mp4", L".m4v", L".webm", L".mov",  L".qt",  L".mkv",
      L".avi", L".wmv", L".asf",  L".flv",  L".mpg", L".mpeg",
      L".mpe", L".mpv", L".m2v",  L".ts",   L".m2ts", L".mts",
      L".3gp", L".3g2", L".ogv",  L".vob"};
  for (std::wstring_view value : kVideoExts) {
    if (ext == value) return true;
  }
  return false;
}

bool isSupportedAudioExtension(std::wstring_view ext) {
  static constexpr std::array<std::wstring_view, 20> kAudioExts = {
      L".wav", L".mp3", L".flac", L".m4a", L".webm",    L".mp4",
      L".mov", L".ogg", L".kss",  L".nsf", L".mid",     L".midi",
      L".vgm", L".vgz", L".psf",  L".minipsf", L".psf2", L".minipsf2",
      L".gsf", L".minigsf"};
  for (std::wstring_view value : kAudioExts) {
    if (ext == value) return true;
  }
  return false;
}

bool findFileSpecificArtwork(const std::wstring& mediaPath,
                             std::wstring* outPath) {
  if (!outPath) return false;
  const std::wstring parent = parentPathOf(mediaPath);
  const std::wstring stem = filenameStemOfPath(mediaPath);
  if (parent.empty() || stem.empty()) return false;

  static constexpr std::array<std::wstring_view, 4> kImageExts = {
      L".jpg", L".jpeg", L".png", L".bmp"};
  for (std::wstring_view ext : kImageExts) {
    const std::wstring candidate = joinPath(parent, stem + std::wstring(ext));
    if (isExistingFile(candidate)) {
      *outPath = candidate;
      return true;
    }
  }
  return false;
}

bool findGenericArtwork(const std::wstring& mediaPath, std::wstring* outPath) {
  if (!outPath) return false;
  const std::wstring parent = parentPathOf(mediaPath);
  if (parent.empty()) return false;

  static constexpr std::array<std::wstring_view, 7> kNames = {
      L"cover", L"folder", L"front", L"album",
      L"albumart", L"albumartsmall", L"albumartlarge"};
  static constexpr std::array<std::wstring_view, 4> kImageExts = {
      L".jpg", L".jpeg", L".png", L".bmp"};

  for (std::wstring_view name : kNames) {
    for (std::wstring_view ext : kImageExts) {
      const std::wstring candidate =
          joinPath(parent, std::wstring(name) + std::wstring(ext));
      if (isExistingFile(candidate)) {
        *outPath = candidate;
        return true;
      }
    }
  }
  return false;
}

bool isExistingDirectory(const std::wstring& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring findRadioifyExecutable() {
  const std::wstring modulePath = getModulePath();
  if (modulePath.empty()) return {};
  const std::wstring candidate = joinPath(parentPathOf(modulePath), L"radioify.exe");
  return isExistingFile(candidate) ? candidate : std::wstring{};
}

std::wstring quoteCommandLineArgument(std::wstring_view value) {
  if (value.empty()) return L"\"\"";

  bool needsQuotes = false;
  for (wchar_t ch : value) {
    if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' ||
        ch == L'"') {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) return std::wstring(value);

  std::wstring quoted;
  quoted.push_back(L'"');
  size_t backslashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::wstring trimTrailingSlashes(std::wstring path) {
  while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
    path.pop_back();
  }
  return path;
}

bool equalPathInsensitive(std::wstring_view left, std::wstring_view right) noexcept {
  const std::wstring lhs = trimTrailingSlashes(std::wstring(left));
  const std::wstring rhs = trimTrailingSlashes(std::wstring(right));
  if (lhs.size() != rhs.size()) return false;
  for (size_t index = 0; index < lhs.size(); ++index) {
    if (std::towlower(lhs[index]) != std::towlower(rhs[index])) return false;
  }
  return true;
}

bool isKnownFolderPath(const std::wstring& path, REFKNOWNFOLDERID folderId) {
  PWSTR knownPath = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &knownPath);
  if (FAILED(hr) || !knownPath || knownPath[0] == L'\0') {
    CoTaskMemFree(knownPath);
    return false;
  }
  const bool matches = equalPathInsensitive(path, knownPath);
  CoTaskMemFree(knownPath);
  return matches;
}

bool isDesktopDirectory(const std::wstring& path) {
  return isKnownFolderPath(path, FOLDERID_Desktop) ||
         isKnownFolderPath(path, FOLDERID_PublicDesktop);
}

HRESULT resolveSinglePathSelection(IShellItemArray* items,
                                   std::wstring* outPath) noexcept {
  if (!items || !outPath) return E_POINTER;
  outPath->clear();

  DWORD count = 0;
  HRESULT hr = items->GetCount(&count);
  if (FAILED(hr)) return hr;
  if (count != 1) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

  ComPtr<IShellItem> item;
  hr = items->GetItemAt(0, item.put());
  if (FAILED(hr)) return hr;

  PWSTR fileSystemPath = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
  if (FAILED(hr) || !fileSystemPath || fileSystemPath[0] == L'\0') {
    CoTaskMemFree(fileSystemPath);
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }

  outPath->assign(fileSystemPath);
  CoTaskMemFree(fileSystemPath);

  if (!isExistingFile(*outPath) && !isExistingDirectory(*outPath)) {
    outPath->clear();
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  return S_OK;
}

HRESULT resolveShellItemDirectory(IShellItem* item, std::wstring* outPath) noexcept {
  if (!item || !outPath) return E_POINTER;
  outPath->clear();

  PWSTR fileSystemPath = nullptr;
  HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
  if (FAILED(hr) || !fileSystemPath || fileSystemPath[0] == L'\0') {
    CoTaskMemFree(fileSystemPath);
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }

  outPath->assign(fileSystemPath);
  CoTaskMemFree(fileSystemPath);
  if (!isExistingDirectory(*outPath)) {
    outPath->clear();
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }
  return S_OK;
}

HRESULT resolveFolderViewDirectory(IUnknown* site, std::wstring* outPath) noexcept {
  if (!site || !outPath) return E_POINTER;
  outPath->clear();

  ComPtr<IServiceProvider> serviceProvider;
  HRESULT hr = site->QueryInterface(IID_PPV_ARGS(serviceProvider.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IShellBrowser> shellBrowser;
  hr = serviceProvider->QueryService(SID_STopLevelBrowser,
                                     IID_PPV_ARGS(shellBrowser.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IShellView> shellView;
  hr = shellBrowser->QueryActiveShellView(shellView.put());
  if (FAILED(hr)) return hr;

  ComPtr<IFolderView> folderView;
  hr = shellView->QueryInterface(IID_PPV_ARGS(folderView.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IPersistFolder2> persistFolder;
  hr = folderView->GetFolder(IID_PPV_ARGS(persistFolder.put()));
  if (FAILED(hr)) return hr;

  PIDLIST_ABSOLUTE folderIdList = nullptr;
  hr = persistFolder->GetCurFolder(&folderIdList);
  if (FAILED(hr) || !folderIdList) {
    CoTaskMemFree(folderIdList);
    return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  }

  ComPtr<IShellItem> folderItem;
  hr = SHCreateItemFromIDList(folderIdList, IID_PPV_ARGS(folderItem.put()));
  CoTaskMemFree(folderIdList);
  if (FAILED(hr)) return hr;

  return resolveShellItemDirectory(folderItem.get(), outPath);
}

HRESULT launchRadioify(const std::wstring& selectedPath) {
  const std::wstring radioifyExe = findRadioifyExecutable();
  if (radioifyExe.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

  const std::wstring arguments =
      quoteCommandLineArgument(L"--shell-open") + L" " +
      quoteCommandLineArgument(selectedPath);
  const std::wstring workingDirectory = parentPathOf(radioifyExe);

  SHELLEXECUTEINFOW executeInfo{};
  executeInfo.cbSize = sizeof(executeInfo);
  executeInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  executeInfo.lpVerb = L"open";
  executeInfo.lpFile = radioifyExe.c_str();
  executeInfo.lpParameters = arguments.c_str();
  executeInfo.lpDirectory =
      workingDirectory.empty() ? nullptr : workingDirectory.c_str();
  executeInfo.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&executeInfo)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

HRESULT convertWicSourceToBitmap(IWICBitmapSource* bitmapSource,
                                 IWICImagingFactory* factory,
                                 HBITMAP* outBitmap,
                                 WTS_ALPHATYPE* outAlpha) {
  if (!bitmapSource || !factory || !outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  ComPtr<IWICBitmapSource> converted;
  WICPixelFormatGUID sourceFormat{};
  HRESULT hr = bitmapSource->GetPixelFormat(&sourceFormat);
  if (FAILED(hr)) return hr;

  if (IsEqualGUID(sourceFormat, GUID_WICPixelFormat32bppBGRA)) {
    hr = bitmapSource->QueryInterface(IID_PPV_ARGS(converted.put()));
  } else {
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (SUCCEEDED(hr)) {
      hr = converter->Initialize(bitmapSource, GUID_WICPixelFormat32bppBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
      hr = converter->QueryInterface(IID_PPV_ARGS(converted.put()));
    }
  }
  if (FAILED(hr)) return hr;

  UINT width = 0;
  UINT height = 0;
  hr = converted->GetSize(&width, &height);
  if (FAILED(hr)) return hr;
  if (width == 0 || height == 0) return E_FAIL;
  if (width > static_cast<UINT>(INT32_MAX) ||
      height > static_cast<UINT>(INT32_MAX)) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  const uint64_t stride64 = static_cast<uint64_t>(width) * 4u;
  const uint64_t bytes64 = stride64 * static_cast<uint64_t>(height);
  if (stride64 > UINT32_MAX || bytes64 > UINT32_MAX) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  BITMAPINFO bitmapInfo{};
  bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
  bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
  bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) return E_OUTOFMEMORY;

  WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  hr = converted->CopyPixels(&rect, static_cast<UINT>(stride64),
                             static_cast<UINT>(bytes64),
                             static_cast<BYTE*>(bits));
  if (FAILED(hr)) {
    DeleteObject(bitmap);
    return hr;
  }

  *outBitmap = bitmap;
  *outAlpha = WTSAT_ARGB;
  return S_OK;
}

HRESULT loadWicDecoderThumbnailBitmap(IWICImagingFactory* factory,
                                      IWICBitmapDecoder* decoder,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!factory || !decoder || !outBitmap || !outAlpha) return E_POINTER;
  if (requestedSize == 0) return E_INVALIDARG;

  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(0, frame.put());
  if (FAILED(hr)) return hr;

  UINT sourceWidth = 0;
  UINT sourceHeight = 0;
  hr = frame->GetSize(&sourceWidth, &sourceHeight);
  if (FAILED(hr)) return hr;
  if (sourceWidth == 0 || sourceHeight == 0) return E_FAIL;

  IWICBitmapSource* source = frame.get();
  ComPtr<IWICBitmapScaler> scaler;
  const UINT maxDimension = sourceWidth > sourceHeight ? sourceWidth : sourceHeight;
  if (maxDimension > requestedSize) {
    const uint64_t targetWidth64 =
        (static_cast<uint64_t>(sourceWidth) * requestedSize + maxDimension / 2) /
        maxDimension;
    const uint64_t targetHeight64 =
        (static_cast<uint64_t>(sourceHeight) * requestedSize + maxDimension / 2) /
        maxDimension;
    const UINT targetWidth = static_cast<UINT>(targetWidth64 > 0 ? targetWidth64 : 1);
    const UINT targetHeight =
        static_cast<UINT>(targetHeight64 > 0 ? targetHeight64 : 1);

    hr = factory->CreateBitmapScaler(scaler.put());
    if (SUCCEEDED(hr)) {
      hr = scaler->Initialize(frame.get(), targetWidth, targetHeight,
                              WICBitmapInterpolationModeFant);
    }
    if (FAILED(hr)) return hr;
    source = scaler.get();
  }

  return convertWicSourceToBitmap(source, factory, outBitmap, outAlpha);
}

HRESULT loadWicThumbnailBitmapFromMemory(const uint8_t* bytes,
                                         size_t byteCount,
                                         UINT requestedSize,
                                         HBITMAP* outBitmap,
                                         WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (!bytes || byteCount == 0 || requestedSize == 0) return E_INVALIDARG;
  if (byteCount > std::numeric_limits<DWORD>::max()) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(stream.put());
  if (FAILED(hr)) return hr;

  hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes),
                                    static_cast<DWORD>(byteCount));
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromStream(
      stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
  if (FAILED(hr)) return hr;

  return loadWicDecoderThumbnailBitmap(factory.get(), decoder.get(),
                                       requestedSize, outBitmap, outAlpha);
}

HRESULT readSharedFileBytes(const std::wstring& path,
                            std::vector<uint8_t>* outBytes) {
  if (!outBytes) return E_POINTER;
  outBytes->clear();
  if (path.empty()) return E_INVALIDARG;

  WinHandle file;
  file.handle =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file.handle == INVALID_HANDLE_VALUE) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  LARGE_INTEGER fileSize{};
  if (!GetFileSizeEx(file.handle, &fileSize)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  if (fileSize.QuadPart <= 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
  if (fileSize.QuadPart > std::numeric_limits<DWORD>::max()) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  outBytes->resize(static_cast<size_t>(fileSize.QuadPart));
  size_t offset = 0;
  while (offset < outBytes->size()) {
    const size_t remaining = outBytes->size() - offset;
    const DWORD toRead = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD bytesRead = 0;
    if (!ReadFile(file.handle, outBytes->data() + offset, toRead, &bytesRead,
                  nullptr)) {
      outBytes->clear();
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (bytesRead == 0) {
      outBytes->clear();
      return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    }
    offset += bytesRead;
  }

  return S_OK;
}

HRESULT loadWicThumbnailBitmap(const std::wstring& imagePath,
                               UINT requestedSize,
                               HBITMAP* outBitmap,
                               WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (imagePath.empty() || requestedSize == 0) return E_INVALIDARG;

  std::vector<uint8_t> bytes;
  HRESULT hr = readSharedFileBytes(imagePath, &bytes);
  if (FAILED(hr)) return hr;
  return loadWicThumbnailBitmapFromMemory(bytes.data(), bytes.size(),
                                          requestedSize, outBitmap, outAlpha);
}

HRESULT openMediaInput(const std::wstring& mediaPath, AvFormatInput* input) {
  if (!input) return E_POINTER;

  std::string pathUtf8;
  HRESULT hr = widePathToUtf8(mediaPath, &pathUtf8);
  if (FAILED(hr)) return hr;

  const int openErr =
      avformat_open_input(input->put(), pathUtf8.c_str(), nullptr, nullptr);
  if (openErr < 0) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);

  const int infoErr = avformat_find_stream_info(input->get(), nullptr);
  if (infoErr < 0) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
  return S_OK;
}

HRESULT loadEmbeddedArtworkBitmap(const std::wstring& mediaPath,
                                  UINT requestedSize,
                                  HBITMAP* outBitmap,
                                  WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  AvFormatInput input;
  HRESULT hr = openMediaInput(mediaPath, &input);
  if (FAILED(hr)) return hr;

  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    AVStream* stream = input->streams[i];
    if (!stream || !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      continue;
    }
    const AVPacket& pic = stream->attached_pic;
    if (!pic.data || pic.size <= 0) {
      continue;
    }
    hr = loadWicThumbnailBitmapFromMemory(
        pic.data, static_cast<size_t>(pic.size), requestedSize, outBitmap,
        outAlpha);
    if (SUCCEEDED(hr)) return hr;
  }
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

int selectThumbnailVideoStream(AVFormatContext* format,
                               const AVCodec** outCodec) {
  if (!format || !outCodec) return -1;
  *outCodec = nullptr;

  int bestIndex = -1;
  int64_t bestPixels = -1;
  bool bestDefault = false;
  const AVCodec* bestCodec = nullptr;

  for (unsigned int i = 0; i < format->nb_streams; ++i) {
    AVStream* stream = format->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
    if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) continue;

    const int64_t pixels = static_cast<int64_t>(stream->codecpar->width) *
                           static_cast<int64_t>(stream->codecpar->height);
    const bool isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
    if (pixels > bestPixels || (pixels == bestPixels && isDefault && !bestDefault)) {
      bestIndex = static_cast<int>(i);
      bestPixels = pixels;
      bestDefault = isDefault;
      bestCodec = codec;
    }
  }

  *outCodec = bestCodec;
  return bestIndex;
}

HRESULT copyVideoFrameToBitmap(const AVFrame* frame,
                               UINT requestedSize,
                               HBITMAP* outBitmap,
                               WTS_ALPHATYPE* outAlpha) {
  if (!frame || !outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (requestedSize == 0 || frame->width <= 0 || frame->height <= 0) {
    return E_INVALIDARG;
  }
  if (requestedSize > static_cast<UINT>(std::numeric_limits<int>::max())) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  const int maxDimension = std::max(frame->width, frame->height);
  const int targetWidth = std::max(
      1, static_cast<int>((static_cast<int64_t>(frame->width) * requestedSize +
                           maxDimension / 2) /
                          maxDimension));
  const int targetHeight = std::max(
      1, static_cast<int>((static_cast<int64_t>(frame->height) * requestedSize +
                           maxDimension / 2) /
                          maxDimension));

  BITMAPINFO bitmapInfo{};
  bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
  bitmapInfo.bmiHeader.biWidth = targetWidth;
  bitmapInfo.bmiHeader.biHeight = -targetHeight;
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) return E_OUTOFMEMORY;

  SwsContextHandle scale;
  scale.ptr = sws_getContext(frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format),
                             targetWidth, targetHeight, AV_PIX_FMT_BGRA,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!scale.ptr) {
    DeleteObject(bitmap);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  uint8_t* dstData[4] = {static_cast<uint8_t*>(bits), nullptr, nullptr,
                         nullptr};
  int dstLinesize[4] = {targetWidth * 4, 0, 0, 0};
  const int scaledRows = sws_scale(scale.ptr, frame->data, frame->linesize, 0,
                                  frame->height, dstData, dstLinesize);
  if (scaledRows != targetHeight) {
    DeleteObject(bitmap);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  uint8_t* pixels = static_cast<uint8_t*>(bits);
  for (int y = 0; y < targetHeight; ++y) {
    uint8_t* row = pixels + static_cast<size_t>(y) * dstLinesize[0];
    for (int x = 0; x < targetWidth; ++x) {
      row[x * 4 + 3] = 0xff;
    }
  }

  *outBitmap = bitmap;
  *outAlpha = WTSAT_ARGB;
  return S_OK;
}

HRESULT sendPacketAndReceiveThumbnail(AVCodecContext* codec,
                                      const AVPacket* packet,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!codec || !outBitmap || !outAlpha) return E_POINTER;
  const int sendErr = avcodec_send_packet(codec, packet);
  if (sendErr < 0 && sendErr != AVERROR_EOF && sendErr != AVERROR(EAGAIN)) {
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  AvFrameHandle frame;
  if (!frame.get()) return E_OUTOFMEMORY;

  for (;;) {
    const int receiveErr = avcodec_receive_frame(codec, frame.get());
    if (receiveErr == AVERROR(EAGAIN) || receiveErr == AVERROR_EOF) {
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (receiveErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

    HRESULT hr =
        copyVideoFrameToBitmap(frame.get(), requestedSize, outBitmap, outAlpha);
    av_frame_unref(frame.get());
    if (SUCCEEDED(hr)) return hr;
  }
}

HRESULT decodeVideoThumbnailFromCurrentPosition(AVFormatContext* format,
                                                AVCodecContext* codec,
                                                int streamIndex,
                                                UINT requestedSize,
                                                HBITMAP* outBitmap,
                                                WTS_ALPHATYPE* outAlpha) {
  if (!format || !codec || streamIndex < 0 || !outBitmap || !outAlpha) {
    return E_POINTER;
  }

  AvPacketHandle packet;
  if (!packet.get()) return E_OUTOFMEMORY;

  constexpr int kMaxThumbnailPackets = 512;
  for (int packetCount = 0; packetCount < kMaxThumbnailPackets; ++packetCount) {
    const int readErr = av_read_frame(format, packet.get());
    if (readErr < 0) break;

    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (packet->stream_index == streamIndex) {
      hr = sendPacketAndReceiveThumbnail(codec, packet.get(), requestedSize,
                                         outBitmap, outAlpha);
    }
    av_packet_unref(packet.get());
    if (SUCCEEDED(hr)) return hr;
  }

  return sendPacketAndReceiveThumbnail(codec, nullptr, requestedSize, outBitmap,
                                       outAlpha);
}

int64_t thumbnailSeekTimestamp(AVFormatContext* format, AVStream* stream) {
  if (!format || !stream) return AV_NOPTS_VALUE;

  if (stream->duration > 0) {
    const int64_t base =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    return base + stream->duration / 10;
  }
  if (format->duration > 0) {
    int64_t target = av_rescale_q(format->duration / 10, kAvTimeBaseQ,
                                  stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE) target += stream->start_time;
    return target;
  }
  return AV_NOPTS_VALUE;
}

HRESULT seekVideoThumbnailPosition(AVFormatContext* format,
                                   AVCodecContext* codec,
                                   int streamIndex,
                                   int64_t timestamp) {
  if (!format || !codec || streamIndex < 0) return E_POINTER;
  if (timestamp == AV_NOPTS_VALUE) return S_FALSE;

  const int seekErr =
      av_seek_frame(format, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
  if (seekErr < 0) return S_FALSE;
  avcodec_flush_buffers(codec);
  return S_OK;
}

HRESULT loadVideoFrameThumbnailBitmap(const std::wstring& mediaPath,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  AvFormatInput input;
  HRESULT hr = openMediaInput(mediaPath, &input);
  if (FAILED(hr)) return hr;

  const AVCodec* codec = nullptr;
  const int streamIndex = selectThumbnailVideoStream(input.get(), &codec);
  if (streamIndex < 0 || !codec) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  AVStream* stream = input->streams[streamIndex];
  AvCodecContextHandle codecContext;
  codecContext.ptr = avcodec_alloc_context3(codec);
  if (!codecContext.get()) return E_OUTOFMEMORY;

  int ffErr = avcodec_parameters_to_context(codecContext.get(), stream->codecpar);
  if (ffErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

  ffErr = avcodec_open2(codecContext.get(), codec, nullptr);
  if (ffErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

  const int64_t seekTimestamp = thumbnailSeekTimestamp(input.get(), stream);
  seekVideoThumbnailPosition(input.get(), codecContext.get(), streamIndex,
                             seekTimestamp);

  hr = decodeVideoThumbnailFromCurrentPosition(input.get(), codecContext.get(),
                                               streamIndex, requestedSize,
                                               outBitmap, outAlpha);
  if (SUCCEEDED(hr)) return hr;
  if (seekTimestamp == AV_NOPTS_VALUE) return hr;

  const int64_t startTimestamp =
      stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
  seekVideoThumbnailPosition(input.get(), codecContext.get(), streamIndex,
                             startTimestamp);
  return decodeVideoThumbnailFromCurrentPosition(input.get(), codecContext.get(),
                                                 streamIndex, requestedSize,
                                                 outBitmap, outAlpha);
}

HRESULT exceptionToHresult() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

class RadioifyExplorerCommand final : public IExplorerCommand,
                                      public IObjectWithSelection,
                                      public IObjectWithSite {
 public:
  RadioifyExplorerCommand() { InterlockedIncrement(&g_objectCount); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
      *object = static_cast<IExplorerCommand*>(this);
    } else if (riid == IID_IObjectWithSelection) {
      *object = static_cast<IObjectWithSelection*>(this);
    } else if (riid == IID_IObjectWithSite) {
      *object = static_cast<IObjectWithSite*>(this);
    } else {
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP SetSelection(IShellItemArray* items) override {
    selection_.reset(items);
    if (items) items->AddRef();
    return S_OK;
  }

  STDMETHODIMP GetSelection(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!selection_) return E_FAIL;
    return selection_->QueryInterface(riid, object);
  }

  STDMETHODIMP SetSite(IUnknown* site) override {
    site_.reset(site);
    if (site) site->AddRef();
    return S_OK;
  }

  STDMETHODIMP GetSite(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!site_) return E_FAIL;
    return site_->QueryInterface(riid, object);
  }

  STDMETHODIMP GetTitle(IShellItemArray*, LPWSTR* title) override {
    return duplicateString(L"Open with Radioify", title);
  }

  STDMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
    try {
      if (!icon) return E_POINTER;
      *icon = nullptr;

      const std::wstring iconPath = joinPath(parentPathOf(getModulePath()), L"radioify.ico");
      if (iconPath.empty() || !isExistingFile(iconPath)) return E_NOTIMPL;
      return duplicateString(iconPath + L",0", icon);
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tooltip) override {
    if (!tooltip) return E_POINTER;
    *tooltip = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetCanonicalName(GUID* commandName) override {
    if (!commandName) return E_POINTER;
    *commandName = radioifyExplorerCommandClsid();
    return S_OK;
  }

  STDMETHODIMP GetState(IShellItemArray* items, BOOL,
                        EXPCMDSTATE* commandState) override {
    try {
      if (!commandState) return E_POINTER;
      std::wstring selectedPath;
      const HRESULT hr =
          resolveTarget(items, &selectedPath);
      *commandState = SUCCEEDED(hr) ? ECS_ENABLED : ECS_HIDDEN;
      return S_OK;
    } catch (...) {
      if (commandState) *commandState = ECS_HIDDEN;
      return S_OK;
    }
  }

  STDMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
    try {
      std::wstring selectedPath;
      HRESULT hr = resolveTarget(items, &selectedPath);
      if (FAILED(hr)) return hr;
      return launchRadioify(selectedPath);
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
    if (!flags) return E_POINTER;
    *flags = ECF_DEFAULT;
    return S_OK;
  }

  STDMETHODIMP EnumSubCommands(IEnumExplorerCommand** enumerator) override {
    if (!enumerator) return E_POINTER;
    *enumerator = nullptr;
    return E_NOTIMPL;
  }

 private:
  ~RadioifyExplorerCommand() { InterlockedDecrement(&g_objectCount); }

  IShellItemArray* resolveSelection(IShellItemArray* items) const noexcept {
    return items ? items : selection_.get();
  }

  HRESULT resolveTarget(IShellItemArray* items, std::wstring* outPath) const noexcept {
    HRESULT hr = resolveSinglePathSelection(resolveSelection(items), outPath);
    if (SUCCEEDED(hr)) return hr;

    hr = resolveFolderViewDirectory(site_.get(), outPath);
    if (FAILED(hr)) return hr;
    if (isDesktopDirectory(*outPath)) {
      outPath->clear();
      return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    return S_OK;
  }

  LONG refCount_ = 1;
  ComPtr<IShellItemArray> selection_;
  ComPtr<IUnknown> site_;
};

class RadioifyExplorerCommandFactory final : public IClassFactory {
 public:
  RadioifyExplorerCommandFactory() { InterlockedIncrement(&g_objectCount); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                              void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;

    RadioifyExplorerCommand* command =
        new (std::nothrow) RadioifyExplorerCommand();
    if (!command) return E_OUTOFMEMORY;

    const HRESULT hr = command->QueryInterface(riid, object);
    command->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_serverLockCount);
    } else {
      InterlockedDecrement(&g_serverLockCount);
    }
    return S_OK;
  }

 private:
  ~RadioifyExplorerCommandFactory() { InterlockedDecrement(&g_objectCount); }

  LONG refCount_ = 1;
};

class RadioifyThumbnailProvider final : public IThumbnailProvider,
                                        public IInitializeWithFile,
                                        public IInitializeWithItem {
 public:
  RadioifyThumbnailProvider() { InterlockedIncrement(&g_objectCount); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) {
      *object = static_cast<IThumbnailProvider*>(this);
    } else if (riid == IID_IInitializeWithFile) {
      *object = static_cast<IInitializeWithFile*>(this);
    } else if (riid == IID_IInitializeWithItem) {
      *object = static_cast<IInitializeWithItem*>(this);
    } else {
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP Initialize(LPCWSTR filePath, DWORD) override {
    try {
      if (!filePath || filePath[0] == L'\0') return E_INVALIDARG;
      if (!mediaPath_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
      if (!isExistingFile(filePath)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
      mediaPath_ = filePath;
      return S_OK;
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP Initialize(IShellItem* item, DWORD) override {
    try {
      if (!item) return E_POINTER;
      if (!mediaPath_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

      PWSTR fileSystemPath = nullptr;
      HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
      if (FAILED(hr) || !fileSystemPath || fileSystemPath[0] == L'\0') {
        CoTaskMemFree(fileSystemPath);
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
      }
      mediaPath_.assign(fileSystemPath);
      CoTaskMemFree(fileSystemPath);

      if (!isExistingFile(mediaPath_)) {
        mediaPath_.clear();
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
      }
      return S_OK;
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap,
                            WTS_ALPHATYPE* alphaType) override {
    try {
      if (!bitmap || !alphaType) return E_POINTER;
      *bitmap = nullptr;
      *alphaType = WTSAT_UNKNOWN;
      if (mediaPath_.empty()) return E_UNEXPECTED;

      const std::wstring ext = extensionOfPath(mediaPath_);
      if (isSupportedImageExtension(ext)) {
        return loadWicThumbnailBitmap(mediaPath_, cx, bitmap, alphaType);
      }

      std::wstring artworkPath;
      if (findFileSpecificArtwork(mediaPath_, &artworkPath)) {
        return loadWicThumbnailBitmap(artworkPath, cx, bitmap, alphaType);
      }

      if (isSupportedVideoExtension(ext)) {
        const HRESULT embeddedHr =
            loadEmbeddedArtworkBitmap(mediaPath_, cx, bitmap, alphaType);
        if (SUCCEEDED(embeddedHr)) return embeddedHr;
        const HRESULT frameHr =
            loadVideoFrameThumbnailBitmap(mediaPath_, cx, bitmap, alphaType);
        if (SUCCEEDED(frameHr)) return frameHr;
        return frameHr;
      }

      if (isSupportedAudioExtension(ext)) {
        const HRESULT embeddedHr =
            loadEmbeddedArtworkBitmap(mediaPath_, cx, bitmap, alphaType);
        if (SUCCEEDED(embeddedHr)) return embeddedHr;
        if (findGenericArtwork(mediaPath_, &artworkPath)) {
          return loadWicThumbnailBitmap(artworkPath, cx, bitmap, alphaType);
        }
        return embeddedHr;
      }

      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    } catch (...) {
      return exceptionToHresult();
    }
  }

 private:
  ~RadioifyThumbnailProvider() { InterlockedDecrement(&g_objectCount); }

  LONG refCount_ = 1;
  std::wstring mediaPath_;
};

class RadioifyThumbnailProviderFactory final : public IClassFactory {
 public:
  RadioifyThumbnailProviderFactory() { InterlockedIncrement(&g_objectCount); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                              void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;

    RadioifyThumbnailProvider* provider =
        new (std::nothrow) RadioifyThumbnailProvider();
    if (!provider) return E_OUTOFMEMORY;

    const HRESULT hr = provider->QueryInterface(riid, object);
    provider->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_serverLockCount);
    } else {
      InterlockedDecrement(&g_serverLockCount);
    }
    return S_OK;
  }

 private:
  ~RadioifyThumbnailProviderFactory() { InterlockedDecrement(&g_objectCount); }

  LONG refCount_ = 1;
};

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason,
                               LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow(void) {
  return (g_objectCount == 0 && g_serverLockCount == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID classId,
                                               REFIID interfaceId,
                                               void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  const bool isCommand =
      InlineIsEqualGUID(classId, radioifyExplorerCommandClsid());
  const bool isThumbnail =
      InlineIsEqualGUID(classId, radioifyExplorerThumbnailClsid());
  if (!isCommand && !isThumbnail) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }

  IClassFactory* factory =
      isCommand
          ? static_cast<IClassFactory*>(new (std::nothrow)
                                            RadioifyExplorerCommandFactory())
          : static_cast<IClassFactory*>(new (std::nothrow)
                                            RadioifyThumbnailProviderFactory());
  if (!factory) return E_OUTOFMEMORY;

  const HRESULT hr = factory->QueryInterface(interfaceId, object);
  factory->Release();
  return hr;
}
