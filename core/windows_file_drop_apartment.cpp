#include "windows_file_drop_apartment.h"

#include <ole2.h>

namespace windows_file_drop {

OleApartment::OleApartment() {
  result_ = OleInitialize(nullptr);
  initialized_ = SUCCEEDED(result_);
}

OleApartment::~OleApartment() {
  if (initialized_) {
    OleUninitialize();
  }
}

}  // namespace windows_file_drop
