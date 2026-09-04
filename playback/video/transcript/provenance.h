#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace playback_video_transcript {

enum class TranscriptProvenanceState : std::uint8_t {
  Missing,
  Matches,
  Mismatch,
  Invalid,
};

struct TranscriptSourceIdentity {
  std::string path;
  std::uintmax_t size = 0;
  std::int64_t modified = 0;
  std::uint64_t device = 0;
  std::uint64_t file = 0;
};

std::filesystem::path transcriptProvenancePath(
    const std::filesystem::path &transcriptPath);

std::optional<TranscriptSourceIdentity> captureTranscriptSourceIdentity(
    const std::filesystem::path &videoPath, std::string *error = nullptr);

bool transcriptSourceMatches(const TranscriptSourceIdentity &expected,
                             const std::filesystem::path &videoPath);

// Writes the commit record for a transcript that is still in staging. The
// source retains filesystem-instance identity; the produced artifact is bound
// to its final path, byte length and SHA-256 content digest. A staging file's
// filesystem identity is deliberately never persisted because replacement
// publication is allowed to give the destination a different file identity.
// Caller publishes both staging files with one TransactionGroup.
bool writeTranscriptProvenanceStaging(
    const TranscriptSourceIdentity &source,
    const std::string &producerIdentity,
    const std::filesystem::path &transcriptDestination,
    const std::filesystem::path &transcriptStaging,
    const std::filesystem::path &provenanceStaging, std::string *error);

TranscriptProvenanceState verifyTranscriptProvenance(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &transcriptPath,
    const std::string &producerIdentity, std::string *error = nullptr);

} // namespace playback_video_transcript
