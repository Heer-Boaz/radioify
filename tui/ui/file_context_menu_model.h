#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class FileContextAction : uint8_t {
  Play,
  BrowseTracks,
  EditVideo,
  CreateIndexedTranscript,
  Analyze,
  SplitLoop,
};

struct FileContextMenuItem {
  FileContextAction action = FileContextAction::Play;
  std::string label;
};

struct FileContextMenuCapabilities {
  bool video = false;
  bool audio = false;
  bool canBrowseTracks = false;
  bool canAnalyze = false;
  bool backgroundTaskRunning = false;
};

std::vector<FileContextMenuItem> buildFileContextMenuItems(
    const FileContextMenuCapabilities& capabilities);
