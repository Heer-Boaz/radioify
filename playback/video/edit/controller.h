#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "playback/video/edit/timeline.h"
#include "playback/video/edit/view.h"

namespace playback_video_edit {

enum class Command : uint8_t {
  Toggle,
  Close,
  MarkIn,
  MarkOut,
  RippleDelete,
  Trim,
  Undo,
  Redo,
  Reset,
  Export,
};

struct CommandContext {
  int64_t sourceDurationUs = 0;
  int64_t playheadUs = 0;
  int64_t frameDurationUs = 1;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
};

struct CommandResult {
  bool handled = false;
  std::optional<int64_t> seekTargetUs;
  std::string message;
};

// Owns the edit session and export worker. It emits explicit effects for the
// playback session to apply, but never controls source playback itself.
class Controller {
 public:
  explicit Controller(std::filesystem::path sourcePath);
  ~Controller();

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  bool active() const;
  CommandResult execute(Command command, const CommandContext& context);
  bool poll(std::string* message);
  void stop();

  const EditSnapshot& edit() const;
  const ExportProgress& exportProgress() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_edit
