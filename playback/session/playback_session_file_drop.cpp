#include "playback_session_file_drop.h"

#include "playback_session_handoff.h"

namespace playback_session_file_drop {

bool handleInputEvent(const playback_session_input::PlaybackInputView& view,
                      playback_session_input::PlaybackInputSignals& signals,
                      const InputEvent& ev) {
  if (ev.type != InputEvent::Type::FileDrop ||
      !isCommittedFileDropEvent(ev.fileDrop)) {
    return false;
  }
  handleFileDrop(view, signals, ev.fileDrop.files);
  return true;
}

void handleFileDrop(const playback_session_input::PlaybackInputView& view,
                    playback_session_input::PlaybackInputSignals& signals,
                    const std::vector<std::filesystem::path>& files) {
  playback_session_handoff::requestOpenFilesHandoff(view, signals, files);
}

}  // namespace playback_session_file_drop
