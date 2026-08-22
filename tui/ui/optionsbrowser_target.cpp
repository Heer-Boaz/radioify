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
    subject.trackIndex = track->trackIndex;
  }
  return subject;
}

BrowserLocation optionsBrowserOpenLocation(
    const OptionsBrowserSubject& subject) {
  return browserOptionsLocation(subject.file, subject.trackIndex.value_or(-1));
}
