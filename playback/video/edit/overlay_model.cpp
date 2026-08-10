#include "playback/video/edit/overlay_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace playback_video_edit {
namespace {

std::string formatTimecode(int64_t timeUs, int64_t frameDurationUs) {
  if (timeUs < 0) return "--:--:--:--";
  const int64_t totalSeconds = timeUs / 1000000;
  const int64_t hours = totalSeconds / 3600;
  const int64_t minutes = (totalSeconds % 3600) / 60;
  const int64_t seconds = totalSeconds % 60;
  char buffer[64];
  if (frameDurationUs > 0) {
    const int64_t nominalFps = std::clamp<int64_t>(
        std::llround(1000000.0 / static_cast<double>(frameDurationUs)), 1, 999);
    const int64_t withinSecondUs = timeUs % 1000000;
    const int64_t frame = std::clamp<int64_t>(
        withinSecondUs * nominalFps / 1000000, 0, nominalFps - 1);
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(frame));
    return std::string(buffer);
  }
  const int64_t milliseconds = (timeUs % 1000000) / 1000;
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld",
                static_cast<long long>(hours),
                static_cast<long long>(minutes),
                static_cast<long long>(seconds),
                static_cast<long long>(milliseconds));
  return std::string(buffer);
}

int timelineCell(int64_t timelineUs, int64_t timelineDurationUs, int width) {
  if (timelineDurationUs <= 0 || width <= 1) return 0;
  const long double ratio =
      static_cast<long double>(timelineUs) / timelineDurationUs;
  return std::clamp(static_cast<int>(std::llround(
                        ratio * static_cast<long double>(width - 1))),
                    0, width - 1);
}

std::string shortestFittingStatus(
    std::initializer_list<const char*> candidates, int width) {
  if (width <= 0) return {};
  for (const char* candidate : candidates) {
    const std::string status(candidate);
    if (static_cast<int>(status.size()) <= width) return status;
  }
  return {};
}

bool appendStatusPart(std::string* status, const std::string& part, int width) {
  if (!status || part.empty() || width <= 0) return false;
  const size_t separatorWidth = status->empty() ? 0 : 2;
  if (status->size() + separatorWidth + part.size() >
      static_cast<size_t>(width)) {
    return false;
  }
  if (separatorWidth != 0) *status += "  ";
  *status += part;
  return true;
}

int availableStatusPartWidth(const std::string& status, int width) {
  const int separatorWidth = status.empty() ? 0 : 2;
  return std::max(0, width - static_cast<int>(status.size()) - separatorWidth);
}

}  // namespace

OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportProgress* editExport, int width,
                               double timelineProgress) {
  OverlayModel model;
  const bool exportRunning = editExport && editExport->running();
  if (width <= 0 ||
      (!edit.active && !edit.exitConfirmation && !exportRunning)) {
    return model;
  }
  if (edit.exitConfirmation) {
    model.status =
        exportRunning
            ? shortestFittingStatus({"EXPORT RUNNING", "EXPORTING", "EXPORT"},
                                    width)
            : shortestFittingStatus({"UNEXPORTED EDITS", "UNEXPORTED", "EDIT*",
                                     "*"},
                                    width);
    return model;
  }
  if (edit.active && edit.timelineDurationUs > 0) {
    model.cells.reserve(static_cast<size_t>(width));
    for (int cell = 0; cell < width; ++cell) {
      const long double ratio =
          static_cast<long double>(2LL * cell + 1) /
          static_cast<long double>(2LL * width);
      const int64_t timelineUs = static_cast<int64_t>(
          ratio * static_cast<long double>(edit.timelineDurationUs));
      const bool selected =
          edit.inTimelineUs && edit.outTimelineUs &&
          timelineUs >= *edit.inTimelineUs && timelineUs < *edit.outTimelineUs;
      model.cells.push_back(selected ? TimelineCellKind::Selected
                                     : TimelineCellKind::Kept);
    }
    model.playheadCell = edit.playheadTimelineUs
                             ? timelineCell(*edit.playheadTimelineUs,
                                            edit.timelineDurationUs, width)
                             : std::clamp(static_cast<int>(std::llround(
                                              std::clamp(timelineProgress, 0.0,
                                                         1.0) *
                                              static_cast<double>(width - 1))),
                                          0, width - 1);
    if (edit.inTimelineUs) {
      model.inCell = timelineCell(*edit.inTimelineUs,
                                  edit.timelineDurationUs, width);
    }
    if (edit.outTimelineUs) {
      model.outCell = timelineCell(*edit.outTimelineUs,
                                   edit.timelineDurationUs, width);
    }
    model.cutCells.reserve(edit.clips.size() > 1 ? edit.clips.size() - 1 : 0);
    for (size_t clip = 1; clip < edit.clips.size(); ++clip) {
      const int cutCell = timelineCell(edit.clips[clip].timelineStartUs,
                                       edit.timelineDurationUs, width);
      if (model.cutCells.empty() || model.cutCells.back() != cutCell) {
        model.cutCells.push_back(cutCell);
      }
    }
  }

  if (exportRunning) {
    const int percentage = static_cast<int>(
        std::lround(std::clamp(editExport->fraction, 0.0, 1.0) * 100.0));
    if (!appendStatusPart(&model.status,
                          "EXPORT " + std::to_string(percentage) + "%",
                          width)) {
      appendStatusPart(
          &model.status,
          shortestFittingStatus({"EXPORTING", "EXPORT", "EXP"}, width),
          width);
    }
  }
  if (edit.active) {
    const int available = availableStatusPartWidth(model.status, width);
    appendStatusPart(
        &model.status,
        edit.dirty ? shortestFittingStatus({"EDIT*", "*"}, available)
                   : shortestFittingStatus({"EDIT"}, available),
        width);
  }
  if (edit.active && edit.playheadTimelineUs) {
    appendStatusPart(
        &model.status,
        "TC " + formatTimecode(*edit.playheadTimelineUs,
                                edit.frameDurationUs) +
            " / " +
            formatTimecode(edit.timelineDurationUs, edit.frameDurationUs),
        width);
  }
  if (edit.active && edit.inTimelineUs) {
    appendStatusPart(
        &model.status,
        "IN " + formatTimecode(*edit.inTimelineUs, edit.frameDurationUs),
        width);
  }
  if (edit.active && edit.outTimelineUs) {
    appendStatusPart(
        &model.status,
        "OUT " + formatTimecode(*edit.outTimelineUs, edit.frameDurationUs),
        width);
  }
  if (edit.active && edit.clips.size() > 1) {
    appendStatusPart(&model.status,
                     "CUTS " + std::to_string(edit.clips.size() - 1), width);
  }
  return model;
}

std::optional<EditBoundary> timelineBoundaryAt(const EditSnapshot& edit,
                                               double timelineRatio,
                                               int timelineWidth,
                                               double grabRadiusCells) {
  if (!edit.active || edit.timelineDurationUs <= 0 || timelineWidth <= 0 ||
      !std::isfinite(timelineRatio) || !std::isfinite(grabRadiusCells) ||
      grabRadiusCells < 0.0) {
    return std::nullopt;
  }
  timelineRatio = std::clamp(timelineRatio, 0.0, 1.0);
  const double span = static_cast<double>(std::max(1, timelineWidth - 1));
  const auto distance = [&](const std::optional<int64_t>& boundaryUs) {
    if (!boundaryUs) return (std::numeric_limits<double>::infinity)();
    const double boundaryRatio = std::clamp(
        static_cast<double>(*boundaryUs) /
            static_cast<double>(edit.timelineDurationUs),
        0.0, 1.0);
    return std::abs(boundaryRatio - timelineRatio) * span;
  };

  const double inDistance = distance(edit.inTimelineUs);
  const double outDistance = distance(edit.outTimelineUs);
  const double nearest = std::min(inDistance, outDistance);
  if (nearest > grabRadiusCells) return std::nullopt;
  return inDistance <= outDistance ? EditBoundary::In : EditBoundary::Out;
}

}  // namespace playback_video_edit
