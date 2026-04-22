#include "windows_file_drop_target.h"

#include <ole2.h>
#include <shellapi.h>

#include <filesystem>
#include <utility>
#include <vector>

namespace windows_file_drop {
namespace {

FORMATETC fileDropFormat() {
  FORMATETC format{};
  format.cfFormat = CF_HDROP;
  format.dwAspect = DVASPECT_CONTENT;
  format.lindex = -1;
  format.tymed = TYMED_HGLOBAL;
  return format;
}

DWORD acceptedEffect(DWORD allowedEffects, bool acceptsData) {
  if (!acceptsData) {
    return DROPEFFECT_NONE;
  }
  if ((allowedEffects & DROPEFFECT_COPY) != 0) {
    return DROPEFFECT_COPY;
  }
  if ((allowedEffects & DROPEFFECT_LINK) != 0) {
    return DROPEFFECT_LINK;
  }
  return DROPEFFECT_NONE;
}

std::vector<std::filesystem::path> readDroppedFiles(HDROP drop) {
  std::vector<std::filesystem::path> files;
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  files.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
    if (length == 0) {
      continue;
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    const UINT copied =
        DragQueryFileW(drop, i, buffer.data(),
                       static_cast<UINT>(buffer.size()));
    if (copied > 0) {
      files.emplace_back(std::wstring(buffer.data(), copied));
    }
  }
  return files;
}

std::vector<std::filesystem::path> filesFromDataObject(IDataObject* dataObject) {
  std::vector<std::filesystem::path> files;
  if (!dataObject) {
    return files;
  }

  FORMATETC format = fileDropFormat();
  STGMEDIUM medium{};
  if (FAILED(dataObject->GetData(&format, &medium))) {
    return files;
  }

  if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop) {
      files = readDroppedFiles(drop);
      GlobalUnlock(medium.hGlobal);
    }
  }
  ReleaseStgMedium(&medium);
  return files;
}

}  // namespace

class DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(DropEventSink sink) : sink_(std::move(sink)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (!object) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IDropTarget)) {
      *object = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count =
        static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject,
                                      DWORD /*keyState*/, POINTL /*point*/,
                                      DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }
    hoveredFiles_ = filesFromDataObject(dataObject);
    acceptsData_ = !hoveredFiles_.empty();
    currentEffect_ = acceptedEffect(*effect, acceptsData_);
    *effect = currentEffect_;
    if (acceptsData_) {
      emitFileDropEvent(FileDropEventPhase::Hover, hoveredFiles_);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD /*keyState*/, POINTL /*point*/,
                                     DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }
    currentEffect_ = acceptedEffect(*effect, acceptsData_);
    *effect = currentEffect_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (acceptsData_) {
      emitFileDropEvent(FileDropEventPhase::Cancel, {});
    }
    hoveredFiles_.clear();
    acceptsData_ = false;
    currentEffect_ = DROPEFFECT_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD /*keyState*/,
                                 POINTL /*point*/, DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }

    std::vector<std::filesystem::path> files = filesFromDataObject(dataObject);
    if (files.empty()) {
      *effect = DROPEFFECT_NONE;
      hoveredFiles_.clear();
      acceptsData_ = false;
      currentEffect_ = DROPEFFECT_NONE;
      return S_OK;
    }

    const DWORD finalEffect = acceptedEffect(*effect, true);
    if (sink_) {
      emitFileDropEvent(FileDropEventPhase::Drop, std::move(files));
      *effect = finalEffect != DROPEFFECT_NONE ? finalEffect : DROPEFFECT_COPY;
    } else {
      *effect = DROPEFFECT_NONE;
    }
    hoveredFiles_.clear();
    acceptsData_ = false;
    currentEffect_ = DROPEFFECT_NONE;
    return S_OK;
  }

 private:
  ~DropTarget() = default;

  void emitFileDropEvent(FileDropEventPhase phase,
                         std::vector<std::filesystem::path> files) {
    if (!sink_) {
      return;
    }
    FileDropEvent ev{};
    ev.phase = phase;
    ev.files = std::move(files);
    sink_(std::move(ev));
  }

  LONG refCount_ = 1;
  DropEventSink sink_;
  std::vector<std::filesystem::path> hoveredFiles_;
  bool acceptsData_ = false;
  DWORD currentEffect_ = DROPEFFECT_NONE;
};

DropTargetRegistration::~DropTargetRegistration() {
  revoke();
}

bool DropTargetRegistration::registerWindow(HWND hwnd, DropEventSink sink) {
  revoke();
  if (!hwnd || !sink) {
    return false;
  }

  oleApartment_.emplace();
  if (!oleApartment_->initialized()) {
    oleApartment_.reset();
    return false;
  }

  target_ = new DropTarget(std::move(sink));
  const HRESULT registerResult = RegisterDragDrop(hwnd, target_);
  if (FAILED(registerResult)) {
    target_->Release();
    target_ = nullptr;
    oleApartment_.reset();
    return false;
  }

  hwnd_ = hwnd;
  return true;
}

void DropTargetRegistration::revoke() {
  if (hwnd_) {
    RevokeDragDrop(hwnd_);
    hwnd_ = nullptr;
  }
  if (target_) {
    target_->Release();
    target_ = nullptr;
  }
  oleApartment_.reset();
}

}  // namespace windows_file_drop
