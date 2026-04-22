#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace videowindow_file_drop {

class FileDropOleApartment {
 public:
  FileDropOleApartment();
  ~FileDropOleApartment();

  FileDropOleApartment(const FileDropOleApartment&) = delete;
  FileDropOleApartment& operator=(const FileDropOleApartment&) = delete;

  bool initialized() const { return initialized_; }
  HRESULT result() const { return result_; }

 private:
  HRESULT result_ = E_UNEXPECTED;
  bool initialized_ = false;
};

}  // namespace videowindow_file_drop
