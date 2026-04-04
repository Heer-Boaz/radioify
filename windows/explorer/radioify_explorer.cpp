#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <wchar.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <new>
#include <string>
#include <vector>

#include "radioify_explorer_config.h"

namespace {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_serverLockCount = 0;
LONG g_contextLogged = 0;

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

  T* detach() {
    T* ptr = ptr_;
    ptr_ = nullptr;
    return ptr;
  }

 private:
  T* ptr_ = nullptr;
};

HRESULT duplicateString(const std::wstring& text, LPWSTR* out) {
  if (!out) return E_POINTER;
  *out = nullptr;

  const size_t length = text.size() + 1;
  wchar_t* buffer =
      static_cast<wchar_t*>(CoTaskMemAlloc(length * sizeof(wchar_t)));
  if (!buffer) return E_OUTOFMEMORY;

  const HRESULT hr = StringCchCopyW(buffer, length, text.c_str());
  if (FAILED(hr)) {
    CoTaskMemFree(buffer);
    return hr;
  }

  *out = buffer;
  return S_OK;
}

std::wstring formatHresult(HRESULT hr) {
  wchar_t buffer[16] = {};
  if (SUCCEEDED(StringCchPrintfW(buffer, std::size(buffer), L"0x%08X",
                                 static_cast<unsigned int>(hr)))) {
    return buffer;
  }
  return L"0x????????";
}

std::wstring formatGuid(REFGUID guid) {
  wchar_t buffer[64] = {};
  const int length = StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  return length > 0 ? std::wstring(buffer, static_cast<size_t>(length - 1))
                    : L"{guid-format-failed}";
}

std::wstring currentProcessPath() {
  std::wstring buffer;
  DWORD size = MAX_PATH;
  for (;;) {
    buffer.resize(size);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), size);
    if (length == 0) return {};
    if (length < size) {
      buffer.resize(length);
      return buffer;
    }
    size *= 2;
    if (size > 32768) return {};
  }
}

std::filesystem::path radioifyExplorerLogPath() {
  wchar_t localAppData[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                               static_cast<DWORD>(std::size(localAppData)));
  if (length > 0 && length < std::size(localAppData)) {
    return std::filesystem::path(localAppData) / "Radioify" / "win11-explorer.log";
  }

  wchar_t tempPath[MAX_PATH] = {};
  const DWORD tempLength =
      GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
  if (tempLength > 0 && tempLength < std::size(tempPath)) {
    return std::filesystem::path(tempPath) / "radioify-win11-explorer.log";
  }

  const std::filesystem::path moduleDir = std::filesystem::path(currentProcessPath()).parent_path();
  return moduleDir.empty() ? std::filesystem::path(L"radioify-win11-explorer.log")
                           : moduleDir / "radioify-win11-explorer.log";
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
  if (length <= 0) return {};

  std::string utf8(static_cast<size_t>(length), '\0');
  const int written =
      WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          utf8.data(), length, nullptr, nullptr);
  if (written != length) return {};
  return utf8;
}

void appendExplorerLog(std::wstring_view message) {
  SYSTEMTIME now{};
  GetLocalTime(&now);

  wchar_t prefix[160] = {};
  if (FAILED(StringCchPrintfW(
          prefix, std::size(prefix),
          L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu ",
          static_cast<unsigned int>(now.wYear),
          static_cast<unsigned int>(now.wMonth),
          static_cast<unsigned int>(now.wDay),
          static_cast<unsigned int>(now.wHour),
          static_cast<unsigned int>(now.wMinute),
          static_cast<unsigned int>(now.wSecond),
          static_cast<unsigned int>(now.wMilliseconds),
          static_cast<unsigned long>(GetCurrentProcessId()),
          static_cast<unsigned long>(GetCurrentThreadId())))) {
    return;
  }

  const std::wstring line = std::wstring(prefix) + std::wstring(message) + L"\r\n";
  OutputDebugStringW((L"[radioify_explorer] " + std::wstring(message) + L"\n").c_str());

  const std::filesystem::path logPath = radioifyExplorerLogPath();
  std::error_code ec;
  std::filesystem::create_directories(logPath.parent_path(), ec);

  const HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  const std::string utf8 = wideToUtf8(line);
  if (!utf8.empty()) {
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  }
  CloseHandle(file);
}

std::filesystem::path currentModuleDirectory();
std::filesystem::path packageExternalLocation();
std::filesystem::path findRadioifyExecutable();
const CLSID& radioifyExplorerCommandClsid();

void logModuleContextOnce() {
  if (InterlockedCompareExchange(&g_contextLogged, 1, 0) != 0) {
    return;
  }

  const std::filesystem::path moduleDir = currentModuleDirectory();
  const std::filesystem::path externalDir = packageExternalLocation();
  const std::filesystem::path executablePath = findRadioifyExecutable();
  appendExplorerLog(L"ProcessContext process=\"" + currentProcessPath() +
                    L"\" moduleDir=\"" + moduleDir.wstring() +
                    L"\" externalLocation=\"" +
                    (externalDir.empty() ? std::wstring(L"<none>")
                                         : externalDir.wstring()) +
                    L"\" radioifyExe=\"" +
                    (executablePath.empty() ? std::wstring(L"<missing>")
                                            : executablePath.wstring()) +
                    L"\" clsid=" + formatGuid(radioifyExplorerCommandClsid()) +
                    L" pointerSize=" + std::to_wstring(sizeof(void*)));
}

bool isExistingDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

bool isExistingRegularFile(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool pathExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

std::wstring describeSelection(IShellItemArray* items) {
  if (!items) return L"selection=null";

  DWORD count = 0;
  HRESULT hr = items->GetCount(&count);
  if (FAILED(hr)) {
    return L"selection.countHr=" + formatHresult(hr);
  }

  std::wstring description = L"selection.count=" + std::to_wstring(count);
  if (count != 1) return description;

  ComPtr<IShellItem> item;
  hr = items->GetItemAt(0, item.put());
  if (FAILED(hr)) {
    return description + L" itemHr=" + formatHresult(hr);
  }

  PWSTR fileSystemPath = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
  if (SUCCEEDED(hr) && fileSystemPath && fileSystemPath[0] != L'\0') {
    description += L" path=\"" + std::wstring(fileSystemPath) + L"\"";
  } else {
    description += L" pathHr=" + formatHresult(hr);
  }
  CoTaskMemFree(fileSystemPath);

  SFGAOF attributes = SFGAO_FOLDER;
  hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
  if (SUCCEEDED(hr)) {
    description += (attributes & SFGAO_FOLDER) != 0 ? L" kind=directory" : L" kind=file";
  } else {
    description += L" attrHr=" + formatHresult(hr);
  }
  return description;
}

std::filesystem::path currentModuleDirectory() {
  if (!g_module) return {};

  std::wstring buffer;
  DWORD size = MAX_PATH;
  for (;;) {
    buffer.resize(size);
    const DWORD length = GetModuleFileNameW(g_module, buffer.data(), size);
    if (length == 0) return {};
    if (length < size) {
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path();
    }
    size *= 2;
    if (size > 32768) return {};
  }
}

std::filesystem::path packageExternalLocation() {
  // Get the current package full name (available when running inside a
  // sparse-package context, e.g. dllhost.exe hosting our surrogate COM class).
  UINT32 nameLength = 0;
  LONG rc = GetCurrentPackageFullName(&nameLength, nullptr);
  if (rc != ERROR_INSUFFICIENT_BUFFER || nameLength == 0) return {};

  std::wstring fullName(nameLength, L'\0');
  rc = GetCurrentPackageFullName(&nameLength, fullName.data());
  if (rc != ERROR_SUCCESS) return {};
  fullName.resize(wcslen(fullName.c_str()));

  // PackagePathType 2 = EffectiveExternal (the ExternalLocation passed to
  // Add-AppxPackage).  GetPackagePathByFullName2 is in kernel32/kernelbase
  // on Windows 10 1903+.
  using GetPackagePathByFullName2_t = LONG(WINAPI*)(PCWSTR, UINT32, UINT32*, PWSTR);
  static const auto pGetPackagePathByFullName2 = reinterpret_cast<GetPackagePathByFullName2_t>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPackagePathByFullName2"));

  if (!pGetPackagePathByFullName2) return {};

  constexpr UINT32 PackagePathType_EffectiveExternal = 2;
  UINT32 pathLength = 0;
  rc = pGetPackagePathByFullName2(fullName.c_str(), PackagePathType_EffectiveExternal,
                                  &pathLength, nullptr);
  if (rc != ERROR_INSUFFICIENT_BUFFER || pathLength == 0) return {};

  std::wstring pathBuffer(pathLength, L'\0');
  rc = pGetPackagePathByFullName2(fullName.c_str(), PackagePathType_EffectiveExternal,
                                  &pathLength, pathBuffer.data());
  if (rc != ERROR_SUCCESS) return {};
  pathBuffer.resize(wcslen(pathBuffer.c_str()));

  return std::filesystem::path(pathBuffer);
}

std::filesystem::path findRadioifyExecutable() {
  const std::filesystem::path moduleDir = currentModuleDirectory();

  std::vector<std::filesystem::path> candidates;

  // The DLL lives in dist/win11-explorer-integration/external-location/.
  // The primary radioify.exe is in dist/, two levels up.
  if (!moduleDir.empty()) {
    candidates.push_back(moduleDir.parent_path().parent_path() / "radioify.exe");
    candidates.push_back(moduleDir / "radioify.exe");
    candidates.push_back(moduleDir.parent_path() / "radioify.exe");
  }

  for (const auto& candidate : candidates) {
    if (isExistingRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

const CLSID& radioifyExplorerCommandClsid() {
  static const CLSID clsid = [] {
    CLSID parsed{};
    const HRESULT hr = CLSIDFromString(
        const_cast<LPWSTR>(RADIOIFY_WIN11_EXPLORER_COMMAND_CLSID_W), &parsed);
    if (FAILED(hr)) {
      return CLSID{};
    }
    return parsed;
  }();
  return clsid;
}

HRESULT getSelectionPath(IShellItemArray* items, std::filesystem::path* outPath,
                         bool* outIsDirectory) {
  if (!items || !outPath || !outIsDirectory) return E_POINTER;
  outPath->clear();
  *outIsDirectory = false;

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

  std::filesystem::path selectedPath(fileSystemPath);
  CoTaskMemFree(fileSystemPath);

  SFGAOF attributes = SFGAO_FOLDER;
  hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
  if (FAILED(hr)) return hr;

  const bool isDirectory = (attributes & SFGAO_FOLDER) != 0;
  if (isDirectory) {
    if (!isExistingDirectory(selectedPath)) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
  } else {
    if (!isExistingRegularFile(selectedPath)) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
  }

  *outPath = selectedPath;
  *outIsDirectory = isDirectory;
  return S_OK;
}

HRESULT launchRadioifyShellOpen(const std::filesystem::path& selectedPath) {
  const std::filesystem::path radioifyExe = findRadioifyExecutable();
  if (!isExistingRegularFile(radioifyExe)) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  if (!pathExists(selectedPath)) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }

  const std::wstring parameters =
      L"--shell-open \"" + selectedPath.wstring() + L"\"";
  SHELLEXECUTEINFOW executeInfo{};
  executeInfo.cbSize = sizeof(executeInfo);
  executeInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  executeInfo.lpVerb = L"open";
  executeInfo.lpFile = radioifyExe.c_str();
  executeInfo.lpParameters = parameters.c_str();
  const std::wstring workingDirectory = radioifyExe.parent_path().wstring();
  executeInfo.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
  executeInfo.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&executeInfo)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

class RadioifyExplorerCommand final : public IExplorerCommand,
                                      public IObjectWithSelection {
 public:
  RadioifyExplorerCommand() { InterlockedIncrement(&g_objectCount); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
      *object = static_cast<IExplorerCommand*>(this);
    } else if (riid == IID_IObjectWithSelection) {
      *object = static_cast<IObjectWithSelection*>(this);
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
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP SetSelection(IShellItemArray* items) override {
    logModuleContextOnce();
    selection_.reset(items);
    if (items) {
      items->AddRef();
    }
    appendExplorerLog(L"SetSelection " + describeSelection(items));
    return S_OK;
  }

  STDMETHODIMP GetSelection(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!selection_) return E_FAIL;
    return selection_->QueryInterface(riid, object);
  }

  STDMETHODIMP GetTitle(IShellItemArray* items, LPWSTR* title) override {
    logModuleContextOnce();
    const HRESULT hr = duplicateString(L"Open with Radioify", title);
    appendExplorerLog(L"GetTitle hr=" + formatHresult(hr) + L" " + describeSelection(items));
    return hr;
  }

  STDMETHODIMP GetIcon(IShellItemArray* items, LPWSTR* icon) override {
    logModuleContextOnce();
    if (!icon) return E_POINTER;
    *icon = nullptr;

    // Look for radioify.ico next to radioify.exe.
    // Icon lives next to the DLL in the external location.
    const std::filesystem::path moduleDir = currentModuleDirectory();
    if (!moduleDir.empty()) {
      const std::filesystem::path icoPath = moduleDir / L"radioify.ico";
      if (isExistingRegularFile(icoPath)) {
        const std::wstring iconResource = icoPath.wstring() + L",0";
        const HRESULT hr = duplicateString(iconResource, icon);
        appendExplorerLog(L"GetIcon hr=" + formatHresult(hr) + L" icon=" +
                          iconResource + L" " + describeSelection(items));
        return hr;
      }
    }

    appendExplorerLog(L"GetIcon hr=" + formatHresult(E_NOTIMPL) + L" ico-not-found " +
                      describeSelection(items));
    return E_NOTIMPL;
  }

  STDMETHODIMP GetToolTip(IShellItemArray* items, LPWSTR* tooltip) override {
    logModuleContextOnce();
    if (!tooltip) return E_POINTER;
    *tooltip = nullptr;
    appendExplorerLog(L"GetToolTip hr=" + formatHresult(E_NOTIMPL) + L" " +
                      describeSelection(items));
    return E_NOTIMPL;
  }

  STDMETHODIMP GetCanonicalName(GUID* commandName) override {
    logModuleContextOnce();
    if (!commandName) return E_POINTER;
    *commandName = radioifyExplorerCommandClsid();
    const HRESULT hr = S_OK;
    appendExplorerLog(L"GetCanonicalName hr=" + formatHresult(hr) +
                      L" clsid=" + formatGuid(*commandName));
    return hr;
  }

  STDMETHODIMP GetState(IShellItemArray* items, BOOL okToBeSlow,
                        EXPCMDSTATE* commandState) override {
    logModuleContextOnce();
    if (!commandState) return E_POINTER;

    IShellItemArray* selection = resolveSelection(items);
    *commandState = ECS_ENABLED;
    appendExplorerLog(L"GetState okToBeSlow=" +
                      std::to_wstring(static_cast<unsigned long>(okToBeSlow)) +
                      L" state=" + std::to_wstring(static_cast<unsigned long>(*commandState)) +
                      L" " + describeSelection(selection));
    return S_OK;
  }

  STDMETHODIMP Invoke(IShellItemArray* items, IBindCtx* bindContext) override {
    logModuleContextOnce();
    (void)bindContext;

    std::filesystem::path selectedPath;
    bool isDirectory = false;
    HRESULT hr = getSelectionPath(resolveSelection(items), &selectedPath,
                                  &isDirectory);
    if (FAILED(hr)) {
      appendExplorerLog(L"Invoke resolveHr=" + formatHresult(hr) + L" " +
                        describeSelection(resolveSelection(items)));
      return hr;
    }

    hr = launchRadioifyShellOpen(selectedPath);
    appendExplorerLog(L"Invoke launchHr=" + formatHresult(hr) + L" path=\"" +
                      selectedPath.wstring() + L"\" isDirectory=" +
                      (isDirectory ? L"true" : L"false"));
    return hr;
  }

  STDMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
    logModuleContextOnce();
    if (!flags) return E_POINTER;
    *flags = ECF_DEFAULT;
    appendExplorerLog(L"GetFlags flags=" + std::to_wstring(static_cast<unsigned long>(*flags)));
    return S_OK;
  }

  STDMETHODIMP EnumSubCommands(IEnumExplorerCommand** enumerator) override {
    if (!enumerator) return E_POINTER;
    *enumerator = nullptr;
    return E_NOTIMPL;
  }

 private:
  ~RadioifyExplorerCommand() { InterlockedDecrement(&g_objectCount); }

  IShellItemArray* resolveSelection(IShellItemArray* items) const {
    return items ? items : selection_.get();
  }

  LONG refCount_ = 1;
  ComPtr<IShellItemArray> selection_;
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
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                              void** object) override {
    logModuleContextOnce();
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) {
      appendExplorerLog(L"Factory.CreateInstance hr=" + formatHresult(CLASS_E_NOAGGREGATION) +
                        L" riid=" + formatGuid(riid) + L" outer=true");
      return CLASS_E_NOAGGREGATION;
    }

    RadioifyExplorerCommand* command =
        new (std::nothrow) RadioifyExplorerCommand();
    if (!command) {
      appendExplorerLog(L"Factory.CreateInstance hr=" + formatHresult(E_OUTOFMEMORY) +
                        L" riid=" + formatGuid(riid) + L" outer=false");
      return E_OUTOFMEMORY;
    }

    const HRESULT hr = command->QueryInterface(riid, object);
    command->Release();
    appendExplorerLog(L"Factory.CreateInstance hr=" + formatHresult(hr) +
                      L" riid=" + formatGuid(riid) + L" outer=false");
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
  logModuleContextOnce();
  const HRESULT hr = (g_objectCount == 0 && g_serverLockCount == 0) ? S_OK : S_FALSE;
  appendExplorerLog(L"DllCanUnloadNow hr=" + formatHresult(hr) + L" objectCount=" +
                    std::to_wstring(static_cast<unsigned long>(g_objectCount)) +
                    L" serverLockCount=" +
                    std::to_wstring(static_cast<unsigned long>(g_serverLockCount)));
  return hr;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID classId,
                                               REFIID interfaceId,
                                               void** object) {
  logModuleContextOnce();
  if (!object) return E_POINTER;
  *object = nullptr;
  if (!InlineIsEqualGUID(classId, radioifyExplorerCommandClsid())) {
    appendExplorerLog(L"DllGetClassObject hr=" + formatHresult(CLASS_E_CLASSNOTAVAILABLE) +
                      L" classId=" + formatGuid(classId) + L" interfaceId=" +
                      formatGuid(interfaceId));
    return CLASS_E_CLASSNOTAVAILABLE;
  }

  RadioifyExplorerCommandFactory* factory =
      new (std::nothrow) RadioifyExplorerCommandFactory();
  if (!factory) return E_OUTOFMEMORY;

  const HRESULT hr = factory->QueryInterface(interfaceId, object);
  factory->Release();
  appendExplorerLog(L"DllGetClassObject hr=" + formatHresult(hr) +
                    L" classId=" + formatGuid(classId) + L" interfaceId=" +
                    formatGuid(interfaceId));
  return hr;
}

extern "C" HRESULT __stdcall DllRegisterServer(void) { return E_NOTIMPL; }

extern "C" HRESULT __stdcall DllUnregisterServer(void) { return E_NOTIMPL; }
