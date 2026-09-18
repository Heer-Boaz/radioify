#include "playback/video/analysis/text_evidence.h"

#include <algorithm>
#include <sstream>

#include "core/sha256.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"

namespace playback_video_analysis {
namespace {

struct CandidateScore {
  int defaultPenalty = 0;
  int hearingImpairedPenalty = 0;
  std::size_t trackIndex = 0;
};

bool better(const CandidateScore &left, const CandidateScore &right) {
  if (left.defaultPenalty != right.defaultPenalty) {
    return left.defaultPenalty < right.defaultPenalty;
  }
  if (left.hearingImpairedPenalty != right.hearingImpairedPenalty) {
    return left.hearingImpairedPenalty < right.hearingImpairedPenalty;
  }
  return left.trackIndex < right.trackIndex;
}

std::string evidenceContentIdentity(const std::string &language,
                                    const std::vector<TextCue> &cues) {
  std::ostringstream identity;
  identity << "timed-speech\n" << language << '\n' << cues.size() << '\n';
  for (const TextCue &cue : cues) {
    identity << cue.startUs << '\n'
             << cue.endUs << '\n'
             << cue.text.size() << '\n';
    identity.write(cue.text.data(),
                   static_cast<std::streamsize>(cue.text.size()));
    identity << '\n';
  }
  return "timed-speech:" + core_sha256::text(identity.str());
}

} // namespace

std::optional<TextEvidence>
selectEnglishTextEvidence(const SubtitleManager &subtitles,
                          const std::filesystem::path &videoPath) {
  return selectEnglishTextEvidence(subtitles.tracks(), videoPath);
}

std::optional<TextEvidence>
selectEnglishTextEvidence(const std::vector<SubtitleTrack> &tracks,
                          const std::filesystem::path &videoPath) {
  const SubtitleTrack *selected = nullptr;
  CandidateScore selectedScore;
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    const SubtitleTrack &track = tracks[index];
    if (!track.textTrack || track.language != "en" || track.cues.empty() ||
        track.forced || track.signsOrSongs || track.commentary) {
      continue;
    }
    if (track.sourceKind == SubtitleTrack::SourceKind::Sidecar &&
        playback_video_transcript::isGeneratedEnglishTranscriptPath(
            videoPath, track.sourcePath)) {
      continue;
    }
    CandidateScore score;
    score.hearingImpairedPenalty = track.hearingImpaired ? 1 : 0;
    score.defaultPenalty = track.defaultDisposition ? 0 : 1;
    score.trackIndex = index;
    if (!selected || better(score, selectedScore)) {
      selected = &track;
      selectedScore = score;
    }
  }
  if (!selected)
    return std::nullopt;

  TextEvidence evidence;
  evidence.language = "en";
  evidence.label = selected->label;
  evidence.cues.reserve(selected->cues.size());
  for (const SubtitleCue &cue : selected->cues) {
    if (cue.text.empty() || cue.endUs <= cue.startUs)
      continue;
    evidence.cues.push_back({cue.startUs, cue.endUs, cue.text});
  }
  if (evidence.cues.empty())
    return std::nullopt;
  evidence.identity = evidenceContentIdentity(evidence.language, evidence.cues);
  return evidence;
}

std::optional<TextEvidence>
loadGeneratedEnglishTextEvidence(const std::filesystem::path &videoPath,
                                 std::string *error) {
  if (error)
    error->clear();
  const std::filesystem::path path =
      playback_video_transcript::generatedEnglishTranscriptPathForVideo(
          videoPath);
  std::error_code filesystemError;
  if (path.empty() ||
      !std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    return std::nullopt;
  }
  std::vector<playback_video_transcript::Segment> segments;
  std::string readError;
  if (!playback_video_transcript::readIndexedTranscript(path, &segments,
                                                        &readError)) {
    if (error) {
      *error = readError.empty()
                   ? "The generated English transcript is unreadable."
                   : std::move(readError);
    }
    return std::nullopt;
  }

  TextEvidence evidence;
  evidence.language = "en";
  evidence.label = "Radioify English transcript";
  evidence.cues.reserve(segments.size());
  for (const playback_video_transcript::Segment &segment : segments) {
    if (segment.text.empty() || segment.endUs <= segment.startUs)
      continue;
    evidence.cues.push_back({segment.startUs, segment.endUs, segment.text});
  }
  if (evidence.cues.empty()) {
    if (error)
      *error = "The generated English transcript contains no speech.";
    return std::nullopt;
  }
  evidence.identity = evidenceContentIdentity(evidence.language, evidence.cues);
  return evidence;
}

} // namespace playback_video_analysis
