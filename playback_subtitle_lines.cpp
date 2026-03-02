#include "playback_frame_output.h"

#include <vector>
#include <string>

namespace playback_frame_output {

std::vector<std::string> extractSubtitleLines(const std::string& subtitleText,
                                            size_t maxLines) {
  std::vector<std::string> lines;
  std::string current;
  if (maxLines == 0) return lines;

  for (char c : subtitleText) {
    if (c == '\r') continue;
    if (c == '\n') {
      if (!current.empty()) {
        lines.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }

  if (lines.size() <= maxLines) {
    return lines;
  }
  const size_t keepFrom = lines.size() - maxLines;
  return std::vector<std::string>(lines.begin() + keepFrom, lines.end());
}

}  // namespace playback_frame_output
