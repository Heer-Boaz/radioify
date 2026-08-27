#pragma once

#include <optional>

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

struct Result {
  Exit exit = Exit::Closed;
  std::optional<OpenFilesRequest> openFiles;
};

Result run(image_viewer_sequence::Sequence sequence, ConsoleInput& input,
           ConsoleScreen& screen, const Style& baseStyle,
           const Style& accentStyle, const Style& dimStyle,
           OpenFileRequests& openFileRequests);

}  // namespace image_viewer
