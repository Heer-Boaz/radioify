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

void appendWrapped(std::vector<std::string> *lines, const std::string &text,
                   int width) {
  if (!lines)
    return;
  std::vector<std::string> wrapped = utf8WrapDisplayWidth(text, width);
  lines->insert(lines->end(), wrapped.begin(), wrapped.end());
}

void appendChapterRow(std::vector<std::string> *lines, const Chapter &chapter,
                      int width) {
  if (!lines || width <= 0)
    return;
  const std::string timestamp =
      playback_video_timeline_preview::formatTimestamp(chapter.startUs);
  const std::string prefix = timestamp + "  ";
  const int prefixWidth = utf8DisplayWidth(prefix);
  if (prefixWidth >= width) {
    lines->push_back(utf8TakeDisplayWidth(prefix, width));
    return;
  }
  std::vector<std::string> titleLines =
      utf8WrapDisplayWidth(chapter.title, width - prefixWidth);
  if (titleLines.empty()) {
    lines->push_back(timestamp);
    return;
  }
  lines->push_back(prefix + titleLines.front());
  const std::string indent(static_cast<std::size_t>(prefixWidth), ' ');
  for (std::size_t index = 1; index < titleLines.size(); ++index) {
    lines->push_back(indent + titleLines[index]);
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
  if (!chapter->summary.empty())
    lines.push_back(chapter->summary);
  return lines;
}

OverviewPanelLayout layoutOverviewPanel(const Snapshot &snapshot, int columns,
                                        int rows, int progressBarY,
                                        int requestedScrollOffset) {
  OverviewPanelLayout out;
  // The overview is a content surface, not an operation-status surface.
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
  std::vector<std::string> documentLines;
  std::vector<std::size_t> documentHeadings;
  std::vector<OverviewPanelLayout::ChapterRow> documentChapterRows;
  documentLines.push_back("Video overview");
  documentHeadings.push_back(0);
  if (!snapshot.overview.empty()) {
    documentLines.push_back({});
    appendWrapped(&documentLines, snapshot.overview, contentWidth);
  }
  documentLines.push_back({});
  documentHeadings.push_back(documentLines.size());
  documentLines.push_back("Chapters");
  for (const Chapter &chapter : snapshot.chapters) {
    const std::size_t firstChapterLine = documentLines.size();
    appendChapterRow(&documentLines, chapter, contentWidth);
    for (std::size_t line = firstChapterLine; line < documentLines.size();
         ++line) {
      documentChapterRows.push_back({line, chapter.startUs});
    }
  }

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
  for (const std::size_t heading : documentHeadings) {
    if (heading >= first && heading < after) {
      out.headingLines.push_back(heading - first);
    }
  }
  for (const OverviewPanelLayout::ChapterRow &row : documentChapterRows) {
    if (row.line >= first && row.line < after)
      out.chapterRows.push_back({row.line - first, row.startUs});
  }
  return out;
}

} // namespace playback_video_chapters
