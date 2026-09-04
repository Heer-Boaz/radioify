#include "playback/video/chapter/presentation.h"

#include <algorithm>
#include <cstdio>

#include "core/unicode_display_width.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_video_chapters {
namespace {

std::string rangeLabel(const Chapter &chapter) {
  return playback_video_timeline_preview::formatTimestamp(chapter.startUs) +
         " - " +
         playback_video_timeline_preview::formatTimestamp(chapter.endUs);
}

using OverviewLine = OverviewPanelLayout::Line;
using OverviewTextRole = OverviewPanelLayout::TextRole;

OverviewLine lineWithRun(std::string text, OverviewTextRole role,
                         int column = 0) {
  OverviewLine line;
  if (!text.empty())
    line.runs.push_back({column, std::move(text), role});
  return line;
}

void appendChapterRow(std::vector<OverviewLine> *lines, const Chapter &chapter,
                      int width) {
  if (!lines || width <= 0)
    return;
  const std::string timestamp =
      playback_video_timeline_preview::formatTimestamp(chapter.startUs);
  const std::string prefix = timestamp + "  ";
  const int prefixWidth = utf8DisplayWidth(prefix);
  if (prefixWidth >= width) {
    OverviewLine line = lineWithRun(utf8TakeDisplayWidth(prefix, width),
                                    OverviewTextRole::Body);
    line.chapterStartUs = chapter.startUs;
    lines->push_back(std::move(line));
    return;
  }
  std::vector<std::string> titleLines =
      utf8WrapDisplayWidth(chapter.title, width - prefixWidth);
  if (titleLines.empty()) {
    OverviewLine line = lineWithRun(timestamp, OverviewTextRole::Body);
    line.chapterStartUs = chapter.startUs;
    lines->push_back(std::move(line));
    return;
  }
  OverviewLine first;
  first.chapterStartUs = chapter.startUs;
  first.runs.push_back({0, prefix, OverviewTextRole::Body});
  first.runs.push_back(
      {prefixWidth, std::move(titleLines.front()), OverviewTextRole::Accent});
  lines->push_back(std::move(first));
  for (std::size_t index = 1; index < titleLines.size(); ++index) {
    OverviewLine continuation = lineWithRun(
        std::move(titleLines[index]), OverviewTextRole::Accent, prefixWidth);
    continuation.chapterStartUs = chapter.startUs;
    lines->push_back(std::move(continuation));
  }
}

} // namespace

std::vector<std::string> previewMetadata(const Snapshot &snapshot,
                                         std::int64_t targetUs) {
  // A timeline hover exists to explain a generated marker. Capability,
  // progress and failure states have no marker metadata and must preserve the
  // existing frame-only preview geometry.
  if (!snapshot.ready())
    return {};

  const Chapter *chapter = chapterAt(snapshot, targetUs);
  if (!chapter)
    return {};
  const auto found =
      std::find_if(snapshot.chapters.begin(), snapshot.chapters.end(),
                   [&](const Chapter &item) { return item.id == chapter->id; });
  const std::size_t index = found == snapshot.chapters.end()
                                ? 0
                                : static_cast<std::size_t>(std::distance(
                                      snapshot.chapters.begin(), found));
  std::vector<std::string> lines;
  lines.push_back("Chapter " + std::to_string(index + 1) + " of " +
                  std::to_string(snapshot.chapters.size()) + " · " +
                  chapter->title);
  lines.push_back(rangeLabel(*chapter));
  return lines;
}

OverviewPanelLayout layoutOverviewPanel(const Snapshot &snapshot, int columns,
                                        int rows, int progressBarY,
                                        int requestedScrollOffset) {
  OverviewPanelLayout out;
  // The chapter list is a content surface, not an operation-status surface.
  if (!snapshot.ready())
    return out;
  const int availableBottom = std::clamp(
      progressBarY > 0 ? progressBarY - 1 : rows - 2, 3, std::max(3, rows - 1));
  if (columns < 24 || rows < 8 || availableBottom < 5)
    return out;

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
    out.height = std::min(availableBottom - out.y + 1, std::max(5, rows / 2));
  }
  if (!out.drawable())
    return OverviewPanelLayout{};

  const int contentWidth = std::max(1, out.width - 4);
  std::vector<OverviewLine> documentLines;
  documentLines.push_back(lineWithRun("Chapters", OverviewTextRole::Accent));
  for (const Chapter &chapter : snapshot.chapters)
    appendChapterRow(&documentLines, chapter, contentWidth);

  out.pageLineCount = std::max(0, out.height - 2);
  out.maximumScrollOffset =
      std::max(0, static_cast<int>(documentLines.size()) - out.pageLineCount);
  out.scrollOffset =
      std::clamp(requestedScrollOffset, 0, out.maximumScrollOffset);
  const std::size_t first = static_cast<std::size_t>(out.scrollOffset);
  const std::size_t after =
      std::min(documentLines.size(),
               first + static_cast<std::size_t>(out.pageLineCount));
  out.lines.assign(documentLines.begin() + static_cast<std::ptrdiff_t>(first),
                   documentLines.begin() + static_cast<std::ptrdiff_t>(after));
  return out;
}

} // namespace playback_video_chapters
