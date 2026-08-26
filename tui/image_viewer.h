#pragma once

#include <functional>

#include "core/open_file_requests.h"
#include "tui/image_viewer_sequence.h"

class ConsoleInput;
class ConsoleScreen;
struct Style;

namespace image_viewer {

enum class Exit {
  Closed,
  QuitRequested,
};

using OpenFilesHandler = std::function<bool(const OpenFilesRequest&)>;

Exit run(image_viewer_sequence::Sequence sequence, ConsoleInput& input,
         ConsoleScreen& screen, const Style& baseStyle,
         const Style& accentStyle, const Style& dimStyle,
         OpenFileRequests& openFileRequests,
         OpenFilesHandler requestOpenFiles);

}  // namespace image_viewer
