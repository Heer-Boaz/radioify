#include "optionsbrowser.h"

#include "media_formats.h"

namespace {

bool supportsOptions(const std::filesystem::path& file) {
  return isKssExt(file) || isGmeExt(file) || isVgmExt(file);
}

}  // namespace

std::optional<OptionsBrowserSubject> optionsBrowserSubjectForEntry(
    const BrowserEntry& entry) {
  if (!entry.isMedia() || entry.path.empty() ||
      !supportsOptions(entry.path)) {
    return std::nullopt;
  }
  OptionsBrowserSubject subject;
  subject.file = entry.path;
  if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
    if (track->trackIndex >= 0) {
      subject.trackIndex = static_cast<uint32_t>(track->trackIndex);
    }
  } else if (isKssExt(entry.path)) {
    constexpr uint32_t kFirstTrackIndex = 0;
    subject.trackIndex = kFirstTrackIndex;
  }
  return subject;
}

BrowserLocation optionsBrowserOpenLocation(
    const OptionsBrowserSubject& subject) {
  return browserOptionsLocation(subject.file, subject.trackIndex);
}
