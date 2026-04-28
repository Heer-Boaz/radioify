#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <strsafe.h>

#include <new>
#include <string>
#include <string_view>

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

void radioifyExplorerSetModule(HMODULE module) noexcept;
std::wstring radioifyExplorerModulePath();

void radioifyExplorerObjectCreated() noexcept;
void radioifyExplorerObjectDestroyed() noexcept;
void radioifyExplorerLockServer(bool lock) noexcept;
bool radioifyExplorerCanUnload() noexcept;

HRESULT duplicateString(std::wstring_view text, LPWSTR* out) noexcept;
HRESULT exceptionToHresult() noexcept;
