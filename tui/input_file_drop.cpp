#include "input_file_drop.h"

bool dispatchFileDrop(
    const InputEvent& ev,
    const std::function<bool(const std::vector<std::filesystem::path>&)>&
        openFiles) {
  if (ev.type != InputEvent::Type::FileDrop ||
      !isCommittedFileDropEvent(ev.fileDrop) || !openFiles) {
    return false;
  }
  return openFiles(ev.fileDrop.files);
}
