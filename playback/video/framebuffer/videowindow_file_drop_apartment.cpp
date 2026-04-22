#include "videowindow_file_drop_apartment.h"

#include <ole2.h>

namespace videowindow_file_drop {

FileDropOleApartment::FileDropOleApartment() {
  result_ = OleInitialize(nullptr);
  initialized_ = SUCCEEDED(result_);
}

FileDropOleApartment::~FileDropOleApartment() {
  if (initialized_) {
    OleUninitialize();
  }
}

}  // namespace videowindow_file_drop
