#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "core/native_wait_handle.h"
#include "playback/video/subtitle/manager.h"

namespace playback_session {

// Shell-owned asynchronous subtitle discovery. A playback session borrows this
// service and owns only its request id, so cancelling or replacing a session
// never joins the discovery worker on the UI thread.
class SubtitleLoadService {
 public:
  using RequestId = std::uint64_t;
  using Operation = std::function<std::optional<SubtitleManager>(
      std::filesystem::path, const SubtitleManager::CancellationCheck&)>;

  struct Completion {
    RequestId requestId = 0;
    std::optional<SubtitleManager> subtitles;
  };

  explicit SubtitleLoadService(Operation operation);
  ~SubtitleLoadService();

  SubtitleLoadService(const SubtitleLoadService&) = delete;
  SubtitleLoadService& operator=(const SubtitleLoadService&) = delete;

  // These methods are owner-thread operations. Starting a new request makes
  // any older request and completion stale.
  std::optional<RequestId> start(std::filesystem::path file);
  bool cancel(RequestId requestId);
  bool active(RequestId requestId) const;
  std::optional<Completion> poll(RequestId requestId);
  NativeWaitHandle waitHandle(RequestId requestId) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SubtitleLoadService> createDefaultSubtitleLoadService();

}  // namespace playback_session
