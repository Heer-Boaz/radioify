#include "file_context_menu_model.h"

std::vector<FileContextMenuItem> buildFileContextMenuItems(
    const FileContextMenuCapabilities& capabilities) {
  std::vector<FileContextMenuItem> items;
  if (capabilities.video) {
    items.push_back({FileContextAction::Play, "Play"});
    items.push_back({FileContextAction::EditVideo, "Edit video"});
    if (!capabilities.backgroundTaskRunning) {
      items.push_back({FileContextAction::CreateIndexedTranscript,
                       "Create indexed transcript"});
    }
    return items;
  }
  if (!capabilities.audio) return items;

  items.push_back({FileContextAction::Play, "Play"});
  if (capabilities.canBrowseTracks) {
    items.push_back({FileContextAction::BrowseTracks, "Browse tracks"});
  }
  if (!capabilities.backgroundTaskRunning) {
    if (capabilities.canAnalyze) {
      items.push_back({FileContextAction::Analyze, "Analyze"});
    }
    items.push_back({FileContextAction::SplitLoop, "Split loop"});
  }
  return items;
}
