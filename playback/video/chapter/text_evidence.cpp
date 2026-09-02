#include "playback/video/chapter/text_evidence.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <string_view>

#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "core/utf8.h"
#include "playback/video/subtitle/manager.h"

namespace playback_video_chapters {
namespace {

struct CandidateScore {
  int rolePenalty = 0;
  int defaultPenalty = 0;
  std::int64_t coveredUs = 0;
  std::size_t textBytes = 0;
  std::size_t trackIndex = 0;
};

bool better(const CandidateScore& left, const CandidateScore& right) {
  if (left.rolePenalty != right.rolePenalty) {
    return left.rolePenalty < right.rolePenalty;
  }
  if (left.coveredUs != right.coveredUs) {
    return left.coveredUs > right.coveredUs;
  }
  if (left.textBytes != right.textBytes) {
    return left.textBytes > right.textBytes;
  }
  if (left.defaultPenalty != right.defaultPenalty) {
    return left.defaultPenalty < right.defaultPenalty;
  }
  return left.trackIndex < right.trackIndex;
}

std::int64_t safeDuration(const SubtitleCue& cue) {
  return cue.endUs > cue.startUs ? cue.endUs - cue.startUs : 0;
}

std::string fileIdentity(const std::filesystem::path& path) {
  if (path.empty()) return {};
  std::error_code error;
  const PathIdentity normalized = makePathIdentity(path);
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  const std::uintmax_t stableSize = error ? 0 : size;
  error.clear();
  const auto modified = std::filesystem::last_write_time(path, error);
  const auto ticks =
      error ? std::int64_t{0}
            : static_cast<std::int64_t>(modified.time_since_epoch().count());
  return toUtf8String(normalized.normalizedPath) + "\n" +
         std::to_string(stableSize) + "\n" + std::to_string(ticks);
}

std::string evidenceIdentity(const SubtitleTrack& track,
                             const std::filesystem::path& videoPath) {
  std::ostringstream identity;
  identity << "radioify-chapter-text-v1\n";
  if (track.sourceKind == SubtitleTrack::SourceKind::Sidecar) {
    identity << "sidecar\n" << fileIdentity(track.sourcePath);
  } else {
    identity << "embedded\n" << fileIdentity(videoPath) << '\n'
             << track.embeddedStreamIndex;
  }
  identity << '\n' << track.language << '\n' << track.label << '\n'
           << track.forced << track.hearingImpaired << track.commentary
           << track.signsOrSongs << track.defaultDisposition << '\n'
           << track.cues.size();
  if (!track.cues.empty()) {
    identity << '\n' << track.cues.front().startUs << '\n'
             << track.cues.back().endUs;
  }
  return identity.str();
}

std::string utf8Prefix(std::string_view value, std::size_t maximumBytes) {
  std::string prefix(value);
  if (!isValidUtf8(prefix)) {
    prefix = wideToUtf8Lossy(utf8ToWideLossy(prefix));
  }
  if (prefix.size() <= maximumBytes) return prefix;
  prefix.resize(maximumBytes);
  while (!prefix.empty() && !isValidUtf8(prefix)) prefix.pop_back();
  return prefix;
}

}  // namespace

std::optional<TextEvidence> selectEnglishTextEvidence(
    const SubtitleManager& subtitles,
    const std::filesystem::path& videoPath) {
  return selectEnglishTextEvidence(subtitles.tracks(), videoPath);
}

std::optional<TextEvidence> selectEnglishTextEvidence(
    const std::vector<SubtitleTrack>& tracks,
    const std::filesystem::path& videoPath) {
  const SubtitleTrack* selected = nullptr;
  CandidateScore selectedScore;
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    const SubtitleTrack& track = tracks[index];
    if (!track.textTrack || track.language != "en" || track.cues.empty()) {
      continue;
    }
    CandidateScore score;
    score.rolePenalty =
        (track.forced ? 8 : 0) + (track.signsOrSongs ? 8 : 0) +
        (track.commentary ? 16 : 0) + (track.hearingImpaired ? 1 : 0);
    score.defaultPenalty = track.defaultDisposition ? 0 : 1;
    score.trackIndex = index;
    for (const SubtitleCue& cue : track.cues) {
      score.coveredUs += safeDuration(cue);
      score.textBytes += cue.text.size();
    }
    if (!selected || better(score, selectedScore)) {
      selected = &track;
      selectedScore = score;
    }
  }
  if (!selected) return std::nullopt;

  TextEvidence evidence;
  evidence.language = "en";
  evidence.label = selected->label;
  evidence.identity = evidenceIdentity(*selected, videoPath);
  evidence.cues.reserve(selected->cues.size());
  for (const SubtitleCue& cue : selected->cues) {
    if (cue.text.empty() || cue.endUs <= cue.startUs) continue;
    evidence.cues.push_back({cue.startUs, cue.endUs, cue.text});
  }
  if (evidence.cues.empty()) return std::nullopt;
  return evidence;
}

std::string textNear(const TextEvidence& evidence, std::int64_t centerUs,
                     std::int64_t radiusUs, std::size_t maxBytes) {
  if (maxBytes == 0 || evidence.cues.empty()) return {};
  radiusUs = std::max<std::int64_t>(0, radiusUs);
  const std::int64_t startUs = std::max<std::int64_t>(0, centerUs - radiusUs);
  const std::int64_t endUs =
      centerUs > (std::numeric_limits<std::int64_t>::max)() - radiusUs
          ? (std::numeric_limits<std::int64_t>::max)()
          : centerUs + radiusUs;
  std::string out;
  for (const TextCue& cue : evidence.cues) {
    if (cue.endUs <= startUs) continue;
    if (cue.startUs >= endUs) break;
    if (!out.empty()) out.push_back(' ');
    const std::size_t available = maxBytes - std::min(maxBytes, out.size());
    if (available == 0) break;
    out += utf8Prefix(cue.text, available);
    if (out.size() >= maxBytes) break;
  }
  return out;
}

std::string textInInterval(const TextEvidence& evidence, std::int64_t startUs,
                           std::int64_t endUs, std::size_t maxBytes) {
  if (maxBytes == 0 || evidence.cues.empty() || startUs < 0 ||
      endUs <= startUs) {
    return {};
  }
  std::string out;
  for (const TextCue& cue : evidence.cues) {
    if (cue.endUs <= cue.startUs) continue;
    const std::int64_t midpointUs = cue.startUs + (cue.endUs - cue.startUs) / 2;
    if (midpointUs < startUs) continue;
    if (midpointUs >= endUs) break;
    if (!out.empty()) out.push_back(' ');
    const std::size_t available = maxBytes - std::min(maxBytes, out.size());
    if (available == 0) break;
    out += utf8Prefix(cue.text, available);
    if (out.size() >= maxBytes) break;
  }
  return out;
}

}  // namespace playback_video_chapters
