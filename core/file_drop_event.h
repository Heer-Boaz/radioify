#pragma once

#include <filesystem>
#include <vector>

enum class FileDropEventPhase {
  Hover,
  Cancel,
  Drop,
};

struct FileDropEvent {
  FileDropEventPhase phase = FileDropEventPhase::Drop;
  std::vector<std::filesystem::path> files;
};

inline bool isCommittedFileDropEvent(const FileDropEvent& event) {
  return event.phase == FileDropEventPhase::Drop && !event.files.empty();
}
