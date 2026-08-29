#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
#include "playback/session/open_outcome.h"

namespace playback_session {

struct OpeningConfiguration {
  std::filesystem::path file;
  bool enableAudio = true;
  bool allowDecoderScale = false;
};

// Asynchronous media-open port. Implementations publish every state edge on
// waitHandles(); requestClose() initiates cleanup and never joins a worker.
class OpeningBackend {
 public:
  virtual ~OpeningBackend() = default;

  virtual std::optional<Problem>
  start(const OpeningConfiguration& configuration) = 0;
  virtual bool initializationDone() = 0;
  virtual bool initializationSucceeded() const = 0;
  virtual std::string initializationError() const = 0;
  virtual bool finishInitialization() = 0;
  virtual void requestClose() = 0;
  virtual bool closeReady() = 0;
  virtual bool finishClose() = 0;
  virtual std::vector<NativeWaitHandle> waitHandles() const = 0;
};

}  // namespace playback_session
