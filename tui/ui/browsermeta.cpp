#include "browsermeta.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <utility>

#include "core/latest_request_worker.h"
#include "playback/video/decoder.h"
#include "ui_helpers.h"

namespace {
std::string formatBytes(uintmax_t bytes) {
  const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int idx = 0;
  while (value >= 1024.0 && idx < 4) {
    value /= 1024.0;
    ++idx;
  }
  char buf[32];
  if (idx == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else if (value >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", value, suffixes[idx]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, suffixes[idx]);
  }
  return buf;
}

std::string formatFileTime(const std::filesystem::file_time_type& ft) {
  using namespace std::chrono;
  auto sctp = time_point_cast<system_clock::duration>(
      ft - std::filesystem::file_time_type::clock::now() +
      system_clock::now());
  std::time_t tt = system_clock::to_time_t(sctp);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) == 0) {
    return "";
  }
  return buf;
}

struct SelectionMetadataRequest {
  std::filesystem::path path;
  bool isDirectory = false;
  bool isVideo = false;
};

struct SelectionMetadataResult {
  std::string sizeLabel;
  std::string timeLabel;
  int width = 0;
  int height = 0;
  int64_t bitRate = 0;
  bool isHDR = false;
  std::string codec;
  std::string duration;
};

using SelectionMetadataWorker =
    LatestRequestWorker<SelectionMetadataRequest, SelectionMetadataResult>;

std::optional<SelectionMetadataResult> loadSelectionMetadata(
    SelectionMetadataRequest request,
    const SelectionMetadataWorker::Cancellation& cancellation) {
  SelectionMetadataResult result;
  try {
    if (request.isDirectory) {
      result.sizeLabel = "<DIR>";
    } else {
      result.sizeLabel = formatBytes(std::filesystem::file_size(request.path));
    }
  } catch (...) {
    result.sizeLabel = "?";
  }
  if (cancellation.requested()) {
    return std::nullopt;
  }

  try {
    result.timeLabel =
        formatFileTime(std::filesystem::last_write_time(request.path));
  } catch (...) {
    result.timeLabel.clear();
  }
  if (cancellation.requested()) {
    return std::nullopt;
  }

  if (request.isVideo) {
    VideoMetadata video;
    std::string error;
    if (probeVideoMetadata(request.path, &video, &error)) {
      result.width = video.width;
      result.height = video.height;
      result.bitRate = video.bitRate;
      result.isHDR = video.isHDR;
      if (video.duration100ns > 0) {
        const double seconds =
            static_cast<double>(video.duration100ns) / 10000000.0;
        result.duration = formatTime(seconds);
      }
      result.codec = std::move(video.codecName);
    }
  }
  if (cancellation.requested()) {
    return std::nullopt;
  }
  return result;
}

bool sameMetadataSelection(const PathIdentity& leftIdentity,
                           bool leftIsDirectory, bool leftIsVideo,
                           const PathIdentity& rightIdentity,
                           bool rightIsDirectory, bool rightIsVideo) {
  return leftIdentity == rightIdentity &&
         leftIsDirectory == rightIsDirectory && leftIsVideo == rightIsVideo;
}
}  // namespace

struct BrowserSelectionMetadata::Impl {
  explicit Impl(IsVideo videoPredicate)
      : isVideo(videoPredicate), worker(loadSelectionMetadata) {}

  void select(const BrowserEntry& entry) {
    const PathIdentity identity =
        entry.pathIdentity.empty() ? makePathIdentity(entry.path)
                                   : entry.pathIdentity;
    const bool directory = entry.isDirectory();
    const bool video = entry.isMedia() && isVideo && isVideo(entry.path);
    if (hasSelection &&
        sameMetadataSelection(selectionIdentity, selectionIsDirectory,
                              selectionIsVideo, identity, directory, video)) {
      return;
    }

    ++generation;
    hasSelection = true;
    selectionIdentity = identity;
    selectionIsDirectory = directory;
    selectionIsVideo = video;
    completedAttempt = false;
    completed.reset();
    worker.submit(generation,
                  SelectionMetadataRequest{entry.path, directory, video});
  }

  void clearSelection() {
    if (!hasSelection) {
      return;
    }
    ++generation;
    hasSelection = false;
    selectionIdentity = {};
    selectionIsDirectory = false;
    selectionIsVideo = false;
    completedAttempt = false;
    completed.reset();
    worker.cancel(generation);
  }

  IsVideo isVideo = nullptr;
  SelectionMetadataWorker worker;
  SelectionMetadataWorker::Generation generation = 0;
  bool hasSelection = false;
  PathIdentity selectionIdentity;
  bool selectionIsDirectory = false;
  bool selectionIsVideo = false;
  bool completedAttempt = false;
  std::optional<SelectionMetadataResult> completed;
};

BrowserSelectionMetadata::BrowserSelectionMetadata(IsVideo isVideo)
    : impl_(std::make_unique<Impl>(isVideo)) {}

BrowserSelectionMetadata::~BrowserSelectionMetadata() = default;

std::string BrowserSelectionMetadata::describe(const BrowserState& browser) {
  if (browser.entries.empty()) {
    impl_->clearSelection();
    return "";
  }
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDirectory() && !entry.actionAs<browser_entry::NavigateUp>()) {
    name += "/";
  }

  if (entry.path.empty()) {
    impl_->clearSelection();
  } else {
    impl_->select(entry);
  }

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  const std::string dirArrow =
      browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  if (!impl_->completedAttempt && !entry.path.empty()) {
    metaLine += "  Loading metadata...";
    return metaLine;
  }
  if (!impl_->completed) {
    return metaLine;
  }

  const SelectionMetadataResult& meta = *impl_->completed;
  if (!meta.sizeLabel.empty()) metaLine += "  " + meta.sizeLabel;
  if (meta.width > 0 && meta.height > 0) {
    metaLine += "  " + std::to_string(meta.width) + "x" +
                std::to_string(meta.height);
    if (meta.isHDR) metaLine += " HDR";
    if (meta.bitRate > 0) {
      metaLine += "  " +
                  formatBytes(static_cast<uintmax_t>(meta.bitRate / 8)) +
                  "/s";
    }
  }
  if (!meta.duration.empty()) metaLine += "  " + meta.duration;
  if (!meta.codec.empty()) metaLine += "  " + meta.codec;
  if (!meta.timeLabel.empty()) metaLine += "  " + meta.timeLabel;
  return metaLine;
}

bool BrowserSelectionMetadata::poll() {
  bool changed = false;
  while (std::optional<SelectionMetadataWorker::Completion> completion =
             impl_->worker.poll()) {
    if (impl_->hasSelection &&
        completion->generation == impl_->generation) {
      impl_->completedAttempt = true;
      impl_->completed = std::move(completion->result);
      changed = true;
    }
  }
  return changed;
}

NativeWaitHandle BrowserSelectionMetadata::nativeWaitHandle() const {
  return impl_->worker.nativeWaitHandle();
}
