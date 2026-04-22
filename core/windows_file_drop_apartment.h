#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace windows_file_drop {

class OleApartment {
 public:
  OleApartment();
  ~OleApartment();

  OleApartment(const OleApartment&) = delete;
  OleApartment& operator=(const OleApartment&) = delete;

  bool initialized() const { return initialized_; }
  HRESULT result() const { return result_; }

 private:
  HRESULT result_ = E_UNEXPECTED;
  bool initialized_ = false;
};

}  // namespace windows_file_drop
