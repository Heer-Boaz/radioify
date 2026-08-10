#include "playback/video/edit/overlay_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playback_video_edit {
namespace {

std::string formatTimestamp(int64_t timeUs) {
  if (timeUs < 0) return "--:--";
  const int64_t totalSeconds = (timeUs + 500000) / 1000000;
  const int64_t hours = totalSeconds / 3600;
  const int64_t minutes = (totalSeconds % 3600) / 60;
  const int64_t seconds = totalSeconds % 60;
  char buffer[64];
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
  }
  return std::string(buffer);
}

bool contains(const EditSnapshot& edit, int64_t sourceUs) {
  for (const SourceRange& range : edit.keptRanges) {
    if (sourceUs >= range.startUs && sourceUs < range.endUs) return true;
  }
  return false;
}

int timelineCell(int64_t sourceUs, int64_t sourceDurationUs, int width) {
  if (sourceDurationUs <= 0 || width <= 1) return 0;
  const long double ratio =
      static_cast<long double>(sourceUs) / sourceDurationUs;
  return std::clamp(static_cast<int>(std::llround(
                        ratio * static_cast<long double>(width - 1))),
                    0, width - 1);
}

}  // namespace

OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportProgress* editExport, int width,
                               double sourceProgress) {
  OverlayModel model;
  const bool exportRunning = editExport && editExport->running();
  if (width <= 0 || (!edit.active && !exportRunning)) return model;
  if (edit.active && edit.sourceDurationUs > 0) {
    model.cells.reserve(static_cast<size_t>(width));
    for (int cell = 0; cell < width; ++cell) {
      const long double ratio =
          static_cast<long double>(2LL * cell + 1) /
          static_cast<long double>(2LL * width);
      const int64_t sourceUs = static_cast<int64_t>(
          ratio * static_cast<long double>(edit.sourceDurationUs));
      const bool selected = edit.inUs && edit.outUs &&
                            sourceUs >= *edit.inUs && sourceUs < *edit.outUs;
      model.cells.push_back(selected ? TimelineCellKind::Selected
                                     : (contains(edit, sourceUs)
                                            ? TimelineCellKind::Kept
                                            : TimelineCellKind::Removed));
    }
    model.playheadCell = std::clamp(
        static_cast<int>(std::llround(std::clamp(sourceProgress, 0.0, 1.0) *
                                     static_cast<double>(width - 1))),
        0, width - 1);
    if (edit.inUs) {
      model.inCell = timelineCell(*edit.inUs, edit.sourceDurationUs, width);
    }
    if (edit.outUs) {
      model.outCell = timelineCell(*edit.outUs, edit.sourceDurationUs, width);
    }
  }

  model.status = edit.active ? " EDIT" : "";
  if (exportRunning) {
    const int percentage = static_cast<int>(
        std::lround(std::clamp(editExport->fraction, 0.0, 1.0) * 100.0));
    model.status += "  EXPORT " + std::to_string(percentage) +
                    "% (Ctrl+E cancel)";
  }
  if (edit.active && edit.inUs) {
    model.status += "  I " + formatTimestamp(*edit.inUs);
  }
  if (edit.active && edit.outUs) {
    model.status += "  O " + formatTimestamp(*edit.outUs);
  }
  if (edit.active) {
    model.status += "  kept " + formatTimestamp(edit.outputDurationUs);
    model.status += "  | I/O mark  Del remove  T trim  Ctrl+E ";
  } else {
    model.status += "  | Ctrl+E ";
  }
  model.status += exportRunning ? "cancel " : "export ";
  return model;
}

}  // namespace playback_video_edit
