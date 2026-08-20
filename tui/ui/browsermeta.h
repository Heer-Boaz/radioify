#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "browser_model.h"
#include "core/native_wait_handle.h"

class BrowserSelectionMetadata {
 public:
  using IsVideo = bool (*)(const std::filesystem::path&);

  explicit BrowserSelectionMetadata(IsVideo isVideo);
  ~BrowserSelectionMetadata();

  BrowserSelectionMetadata(const BrowserSelectionMetadata&) = delete;
  BrowserSelectionMetadata& operator=(const BrowserSelectionMetadata&) =
      delete;

  // Called by the UI owner. Schedules unseen selection metadata and returns
  // immediately using only already completed data.
  std::string describe(const BrowserState& browser);
  bool poll();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
