#include "playback/video/transcript/subtitle_cues.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "playback/video/transcript/cue_semantics.h"

namespace playback_video_transcript {
namespace {

constexpr int64_t kSplitSilenceUs = 800'000;
constexpr int64_t kMaximumCueDurationUs = 7'000'000;
constexpr int64_t kCueTailUs = 150'000;
constexpr int64_t kAlignmentLeadUs = 200'000;
constexpr int64_t kAlignmentTailUs = 350'000;
constexpr int64_t kPreferredCueDurationUs = 4'500'000;
constexpr size_t kMaximumCueCharacters = 80;
constexpr size_t kPreferredCueCharacters = 48;

struct TimedPiece {
  int64_t startUs = -1;
  int64_t endUs = -1;
  int64_t firstAlignmentUs = -1;
  int64_t lastAlignmentUs = -1;
  std::string text;
};

size_t utf8Characters(const std::string& text) {
  size_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xc0u) != 0x80u) ++count;
  }
  return count;
}

bool startsWithWhitespace(const std::string& text) {
  return !text.empty() &&
         (text.front() == ' ' || text.front() == '\t' ||
          text.front() == '\r' || text.front() == '\n');
}

bool endsSentence(const std::string& text) {
  const auto last = std::find_if_not(
      text.rbegin(), text.rend(), [](unsigned char byte) {
        return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
      });
  return last != text.rend() &&
         (*last == '.' || *last == '!' || *last == '?');
}

int64_t saturatingAdd(int64_t value, int64_t increment) {
  if (increment > 0 &&
      value > (std::numeric_limits<int64_t>::max)() - increment) {
    return (std::numeric_limits<int64_t>::max)();
  }
  return value + increment;
}

std::vector<TimedPiece> timedPieces(const RecognizedSegment& segment) {
  std::vector<TimedPiece> tokens;
  tokens.reserve(segment.tokens.size());
  TimedPiece pending;

  for (const RecognizedToken& token : segment.tokens) {
    if (token.text.empty()) continue;
    const bool validTiming =
        token.startUs >= segment.startUs && token.endUs > token.startUs &&
        token.endUs <= segment.endUs;
    pending.text += token.text;
    if (validTiming) {
      if (pending.startUs < 0) pending.startUs = token.startUs;
      pending.endUs = std::max(pending.endUs, token.endUs);
    }
    const bool validAlignment = token.alignmentUs >= segment.startUs &&
                                token.alignmentUs <= segment.endUs;
    if (validAlignment) {
      if (pending.firstAlignmentUs < 0) {
        pending.firstAlignmentUs = token.alignmentUs;
      }
      pending.lastAlignmentUs =
          std::max(pending.lastAlignmentUs, token.alignmentUs);
    }
    // Whisper tokens may divide the bytes of one UTF-8 code point. Keep those
    // bytes together so a cue boundary can never create invalid UTF-8.
    if (!isValidUtf8(pending.text)) continue;
    if (pending.startUs < 0) {
      if (!tokens.empty()) {
        tokens.back().text += pending.text;
        pending = {};
      }
      continue;
    }
    tokens.push_back(std::move(pending));
    pending = {};
  }

  if (!pending.text.empty() && !tokens.empty()) {
    tokens.back().text += pending.text;
  }

  std::vector<TimedPiece> words;
  words.reserve(tokens.size());
  for (TimedPiece& token : tokens) {
    if (words.empty() || startsWithWhitespace(token.text)) {
      words.push_back(std::move(token));
    } else {
      words.back().endUs = std::max(words.back().endUs, token.endUs);
      if (words.back().firstAlignmentUs < 0) {
        words.back().firstAlignmentUs = token.firstAlignmentUs;
      }
      words.back().lastAlignmentUs =
          std::max(words.back().lastAlignmentUs, token.lastAlignmentUs);
      words.back().text += token.text;
    }
  }
  for (TimedPiece& word : words) {
    if (word.firstAlignmentUs < 0 || word.lastAlignmentUs < 0) continue;
    word.startUs = std::max(segment.startUs,
                            word.firstAlignmentUs - kAlignmentLeadUs);
    word.endUs = std::min(segment.endUs,
                          saturatingAdd(word.lastAlignmentUs,
                                        kAlignmentTailUs));
    if (word.endUs <= word.startUs) {
      word.endUs = std::min(segment.endUs,
                            saturatingAdd(word.startUs, kAlignmentTailUs));
    }
  }
  return words;
}

void appendCue(std::vector<Segment>* cues, Segment* cue) {
  if (!cues || !cue || cue->endUs <= cue->startUs ||
      !isTranscriptSpeechCue(cue->text)) {
    if (cue) *cue = {};
    return;
  }
  cue->endUs = std::min(
      saturatingAdd(cue->startUs, kMaximumCueDurationUs),
      saturatingAdd(cue->endUs, kCueTailUs));
  cues->push_back(std::move(*cue));
  *cue = {};
}

void appendSegmentCues(const RecognizedSegment& segment,
                       std::vector<Segment>* cues) {
  if (!cues || segment.endUs <= segment.startUs ||
      !isTranscriptSpeechCue(segment.text)) {
    return;
  }
  const std::vector<TimedPiece> pieces = timedPieces(segment);
  if (pieces.empty()) {
    Segment fallback{segment.startUs, segment.endUs, segment.text};
    appendCue(cues, &fallback);
    return;
  }

  Segment cue;
  size_t characters = 0;
  for (const TimedPiece& piece : pieces) {
    const size_t pieceCharacters = utf8Characters(piece.text);
    const bool splitForSilence =
        !cue.text.empty() && piece.startUs - cue.endUs >= kSplitSilenceUs;
    const bool splitForDuration =
        !cue.text.empty() &&
        piece.endUs - cue.startUs > kMaximumCueDurationUs;
    const bool splitForLength =
        !cue.text.empty() &&
        characters + pieceCharacters > kMaximumCueCharacters;
    const bool splitAtSentenceBoundary =
        !cue.text.empty() && endsSentence(cue.text) &&
        (characters >= kPreferredCueCharacters ||
         cue.endUs - cue.startUs >= kPreferredCueDurationUs);
    if (splitForSilence || splitForDuration || splitForLength ||
        splitAtSentenceBoundary) {
      appendCue(cues, &cue);
      characters = 0;
    }
    if (cue.text.empty()) {
      cue.startUs = piece.startUs;
      cue.endUs = piece.endUs;
    } else {
      cue.endUs = std::max(cue.endUs, piece.endUs);
    }
    cue.text += piece.text;
    characters += pieceCharacters;
  }
  appendCue(cues, &cue);
}

}  // namespace

std::vector<Segment> buildSubtitleCues(
    const std::vector<RecognizedSegment>& recognition) {
  std::vector<Segment> cues;
  for (const RecognizedSegment& segment : recognition) {
    appendSegmentCues(segment, &cues);
  }
  finalizeSubtitleCueTimeline(&cues);
  return cues;
}

void finalizeSubtitleCueTimeline(std::vector<Segment>* cues) {
  if (!cues) return;
  cues->erase(std::remove_if(cues->begin(), cues->end(),
                             [](const Segment& cue) {
                               return cue.startUs < 0 ||
                                      cue.endUs <= cue.startUs ||
                                      !isTranscriptSpeechCue(cue.text);
                             }),
              cues->end());
  std::stable_sort(cues->begin(), cues->end(),
                   [](const Segment& lhs, const Segment& rhs) {
                     if (lhs.startUs != rhs.startUs) {
                       return lhs.startUs < rhs.startUs;
                     }
                     return lhs.endUs < rhs.endUs;
                   });
  for (size_t index = 1; index < cues->size(); ++index) {
    Segment& previous = (*cues)[index - 1];
    const Segment& current = (*cues)[index];
    if (previous.endUs > current.startUs &&
        current.startUs > previous.startUs) {
      previous.endUs = current.startUs;
    }
  }
}

}  // namespace playback_video_transcript
