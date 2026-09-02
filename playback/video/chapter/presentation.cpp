#include "playback/video/chapter/presentation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/unicode_display_width.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_video_chapters {
namespace {

std::string percentage(double value) {
  const int percent = static_cast<int>(
      std::lround(std::clamp(value, 0.0, 1.0) * 100.0));
  return std::to_string(percent) + "%";
}

std::string rangeLabel(const Chapter& chapter) {
  return playback_video_timeline_preview::formatTimestamp(chapter.startUs) +
         " - " +
         playback_video_timeline_preview::formatTimestamp(chapter.endUs);
}

std::vector<std::string> wrapLine(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0 || text.empty()) return lines;
  std::string remaining = text;
  while (!remaining.empty()) {
    if (utf8DisplayWidth(remaining) <= width) {
      lines.push_back(std::move(remaining));
      break;
    }
    std::string candidate = utf8TakeDisplayWidth(remaining, width);
    std::size_t split = candidate.find_last_of(" \t");
    if (split == std::string::npos || split == 0) {
      split = candidate.size();
    }
    std::string line = candidate.substr(0, split);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (!line.empty()) lines.push_back(std::move(line));
    remaining.erase(0, split);
    while (!remaining.empty() &&
           (remaining.front() == ' ' || remaining.front() == '\t')) {
      remaining.erase(remaining.begin());
    }
  }
  return lines;
}

void appendWrapped(std::vector<std::string>* lines, const std::string& text,
                   int width, int limit = 0) {
  if (!lines) return;
  std::vector<std::string> wrapped = wrapLine(text, width);
  if (limit > 0 && static_cast<int>(wrapped.size()) > limit) {
    wrapped.resize(static_cast<std::size_t>(limit));
    if (!wrapped.empty() && width > 1) {
      std::string& last = wrapped.back();
      last = utf8TakeDisplayWidth(last, width - 1) + "…";
    }
  }
  lines->insert(lines->end(), wrapped.begin(), wrapped.end());
}

}  // namespace

std::string stateStatusLine(const Snapshot& snapshot) {
  const std::string label = analysisStateLabel(snapshot.state);
  std::string line = label;
  if (snapshot.progress) line += " · " + percentage(*snapshot.progress);
  if (!snapshot.phase.empty() && snapshot.phase != label) {
    line += " · " + snapshot.phase;
  }
  return line;
}

std::vector<std::string> previewMetadata(const Snapshot& snapshot,
                                         std::int64_t targetUs) {
  // Unsupported is a terminal capability result, not useful hover metadata.
  // Returning no metadata lets the shared preview layout collapse to the
  // existing frame-only popover instead of reserving an empty text column.
  if (snapshot.state == AnalysisState::Unsupported) return {};

  if (snapshot.ready()) {
    const Chapter* chapter = chapterAt(snapshot, targetUs);
    if (!chapter) return {};
    const auto found = std::find_if(
        snapshot.chapters.begin(), snapshot.chapters.end(),
        [&](const Chapter& item) { return item.id == chapter->id; });
    const std::size_t index =
        found == snapshot.chapters.end()
            ? 0
            : static_cast<std::size_t>(
                  std::distance(snapshot.chapters.begin(), found));
    std::vector<std::string> lines;
    lines.push_back("Chapter " + std::to_string(index + 1) + " of " +
                    std::to_string(snapshot.chapters.size()) + " · " +
                    chapter->title);
    lines.push_back(rangeLabel(*chapter));
    if (!chapter->summary.empty()) lines.push_back(chapter->summary);
    return lines;
  }

  std::vector<std::string> lines{stateStatusLine(snapshot)};
  if (!snapshot.detail.empty()) lines.push_back(snapshot.detail);
  return lines;
}

OverviewPanelLayout layoutOverviewPanel(const Snapshot& snapshot,
                                        int columns, int rows,
                                        int progressBarY) {
  OverviewPanelLayout out;
  const int availableBottom =
      std::clamp(progressBarY > 0 ? progressBarY - 1 : rows - 2, 3,
                 std::max(3, rows - 1));
  if (columns < 24 || rows < 8 || availableBottom < 5) return out;

  out.drawer = columns >= 96 && rows >= 16;
  if (out.drawer) {
    out.width = std::clamp(columns / 3, 34, 48);
    out.x = columns - out.width - 1;
    out.y = 1;
    out.height = std::min(availableBottom - out.y + 1, rows - 2);
  } else {
    out.x = 1;
    out.y = 1;
    out.width = columns - 2;
    out.height = std::min(availableBottom - out.y + 1,
                          std::max(5, rows / 2));
  }
  if (!out.drawable()) return OverviewPanelLayout{};

  const int contentWidth = std::max(1, out.width - 4);
  out.lines.push_back("Video overview");
  out.lines.push_back(stateStatusLine(snapshot));
  if (!snapshot.detail.empty() && !snapshot.ready()) {
    appendWrapped(&out.lines, snapshot.detail, contentWidth, 2);
  }
  if (!snapshot.overview.empty()) {
    out.lines.push_back({});
    appendWrapped(&out.lines, snapshot.overview, contentWidth,
                  out.drawer ? 4 : 2);
  }
  if (snapshot.ready()) {
    out.lines.push_back({});
    for (const Chapter& chapter : snapshot.chapters) {
      const std::string row =
          playback_video_timeline_preview::formatTimestamp(chapter.startUs) +
          "  " + chapter.title;
      appendWrapped(&out.lines, row, contentWidth, 1);
    }
  }

  const int lineCapacity = std::max(0, out.height - 2);
  if (static_cast<int>(out.lines.size()) > lineCapacity) {
    out.lines.resize(static_cast<std::size_t>(lineCapacity));
    if (!out.lines.empty() && contentWidth > 1) {
      out.lines.back() = utf8TakeDisplayWidth(out.lines.back(),
                                              contentWidth - 1) + "…";
    }
  }
  return out;
}

}  // namespace playback_video_chapters
