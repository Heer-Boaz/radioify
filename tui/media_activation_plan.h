#pragma once

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "app/playback_route.h"
#include "core/open_file_requests.h"
#include "tui/image_viewer_sequence.h"

namespace tui_media_activation {

enum class DefaultVideoPresentation {
  TerminalAscii,
  NativeWindowedFramebuffer,
};

struct QueueFiles {
  playback_route::Route route;
  std::vector<std::filesystem::path> files;
};

struct ShowImages {
  playback_route::Route route;
  image_viewer_sequence::Sequence sequence;
};

struct OpenDirectory {
  std::filesystem::path path;
};

struct Failure {
  std::string message;
};

using Plan = std::variant<QueueFiles, ShowImages, OpenDirectory, Failure>;

Plan planFiles(playback_route::Route route,
               const std::vector<std::filesystem::path>& files);
Plan planOpenFiles(const OpenFilesRequest& request,
                   DefaultVideoPresentation defaultVideoPresentation);

}  // namespace tui_media_activation
