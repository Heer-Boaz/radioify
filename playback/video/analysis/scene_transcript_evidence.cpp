#include "playback/video/analysis/scene_transcript_evidence.h"

#include <utility>

#include "playback/video/transcript/document.h"

namespace playback_video_analysis {

bool loadSceneTranscriptEvidence(
    const std::filesystem::path& transcriptPath,
    std::vector<playback_video_transcript::Segment>* segments,
    std::string* error) {
  if (error) error->clear();
  if (!segments) {
    if (error) *error = "Scene transcript destination is empty.";
    return false;
  }
  segments->clear();
  if (transcriptPath.empty()) return true;

  std::string transcriptError;
  if (playback_video_transcript::readIndexedTranscript(
          transcriptPath, segments, &transcriptError)) {
    return true;
  }
  segments->clear();
  if (error) {
    *error = "Scene analysis cannot use the indexed transcript: " +
             (transcriptError.empty()
                  ? std::string("the sidecar is unreadable.")
                  : std::move(transcriptError));
  }
  return false;
}

}  // namespace playback_video_analysis
