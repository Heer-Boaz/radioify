#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"

namespace playback_video_analysis {

enum class VisualTimelineScanStatus : std::uint8_t {
  Succeeded,
  Cancelled,
  Yielded,
  Unsupported,
  Failed,
};

struct VisualTimelineScanRequest {
  std::filesystem::path videoPath;
  int videoStreamIndex = -1;
  std::int64_t expectedDurationUs = 0;
  bool requireD3d11 = false;
};

struct VisualTimelineScanControl {
  std::function<bool()> cancelled;
  // A scan that requires D3D11 must cooperatively yield whenever the
  // foreground playback owner revokes background GPU admission.
  std::function<bool()> backgroundGpuAllowed;
  std::function<void(double)> progress;
};

struct VisualTimelineScanResult {
  VisualTimelineScanStatus status = VisualTimelineScanStatus::Failed;
  std::string detail;
  std::int64_t durationUs = 0;
  std::vector<VisualSample> samples;
};

// Owner-held continuation for a cooperative D3D11 scan. The checkpoint is
// request-bound and only contains fully published samples; callers may safely
// keep it across foreground-playback GPU revocation. A successful scan
// consumes the checkpoint into VisualTimelineScanResult.
struct VisualTimelineScanCheckpoint {
  bool initialized = false;
  std::filesystem::path videoPath;
  int videoStreamIndex = -1;
  std::int64_t expectedDurationUs = 0;
  bool requireD3d11 = false;
  std::int64_t durationUs = 0;
  std::int64_t nextSampleUs = 0;
  std::vector<VisualSample> samples;
};

bool validVisualTimelineScanCheckpoint(
    const VisualTimelineScanRequest &request,
    const VisualTimelineScanCheckpoint &checkpoint);

// Sequentially decodes the complete source timeline and extracts a bounded
// luma signature every kVisualSampleIntervalUs. This is the shared temporal
// evidence owner for edit suggestions and semantic chaptering: callers may
// interpret the samples differently, but neither may replace the scan with a
// few random-access thumbnails.
VisualTimelineScanResult
scanVisualTimeline(const VisualTimelineScanRequest &request,
                   const VisualTimelineScanControl &control,
                   VisualTimelineScanCheckpoint *checkpoint = nullptr);

} // namespace playback_video_analysis
