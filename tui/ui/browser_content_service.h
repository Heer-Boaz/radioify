#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "browser_navigation.h"
#include "core/native_wait_handle.h"

class AudioPlaybackRuntime;

// Owns asynchronous browser-content preparation. BrowserNavigator owns the
// navigation state machine; this service owns only worker lifetime and the
// runtime snapshot needed to prepare Options pages.
class BrowserContentService final : public BrowserPreparationService {
 public:
  struct Config {
    AudioPlaybackRuntime& audioPlayback;
    int sampleRate = 0;
    std::uint32_t outputChannels = 0;
  };

  struct Completion {
    BrowserPreparationId generation = 0;
    std::optional<BrowserPreparationResult> result;
  };

  explicit BrowserContentService(Config config);
  ~BrowserContentService() override;

  BrowserContentService(const BrowserContentService&) = delete;
  BrowserContentService& operator=(const BrowserContentService&) = delete;

  BrowserContentPreparation prepare(
      BrowserPreparationId preparationId,
      const BrowserContentRequest& request) override;
  void cancelThrough(BrowserPreparationId generation) override;

  std::optional<Completion> poll();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
