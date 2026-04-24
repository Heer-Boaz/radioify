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

#include <cwctype>
#include <new>
#include <string>
#include <string_view>

#include "radioify_explorer_config.h"

namespace {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_serverLockCount = 0;

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
  if (!InlineIsEqualGUID(classId, radioifyExplorerCommandClsid())) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }

  RadioifyExplorerCommandFactory* factory =
      new (std::nothrow) RadioifyExplorerCommandFactory();
  if (!factory) return E_OUTOFMEMORY;

  const HRESULT hr = factory->QueryInterface(interfaceId, object);
  factory->Release();
  return hr;
}
