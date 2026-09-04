#include "playback/video/transcript/provenance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

#include "core/file_instance.h"
#include "core/runtime_helpers.h"
#include "core/sha256.h"

namespace playback_video_transcript {
namespace {

constexpr int kSchema = 3;
constexpr std::uintmax_t kMaximumManifestBytes = 64u * 1024u;

struct FileIdentity {
  std::string path;
  std::uintmax_t size = 0;
  std::int64_t modified = 0;
  std::uint64_t device = 0;
  std::uint64_t file = 0;
};

struct ArtifactIdentity {
  std::string path;
  std::uintmax_t size = 0;
  std::string sha256;
};

void setError(std::string *error, std::string value) {
  if (error)
    *error = std::move(value);
}

bool identityFor(const std::filesystem::path &path, FileIdentity *identity,
                 const std::filesystem::path &identityPath = {}) {
  if (!identity)
    return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return false;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error)
    return false;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error)
    return false;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(
          identityPath.empty() ? path : identityPath, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(
                     identityPath.empty() ? path : identityPath, error)
                     .lexically_normal();
  }
  if (error || normalized.empty())
    return false;
  identity->path = toUtf8String(normalized);
  identity->size = size;
  identity->modified =
      static_cast<std::int64_t>(modified.time_since_epoch().count());
  if (const auto instance = fileInstanceIdentity(path)) {
    identity->device = instance->device;
    identity->file = instance->file;
  }
  return true;
}

nlohmann::json encode(const FileIdentity &identity) {
  return {{"path", identity.path},
          {"size", identity.size},
          {"modified", identity.modified},
          {"device", identity.device},
          {"file", identity.file}};
}

bool decode(const nlohmann::json &document, FileIdentity *identity) {
  if (!identity || !document.is_object() ||
      !document.contains("path") || !document["path"].is_string() ||
      !document.contains("size") ||
      !document["size"].is_number_unsigned() ||
      !document.contains("modified") ||
      !document["modified"].is_number_integer() ||
      !document.contains("device") ||
      !document["device"].is_number_unsigned() ||
      !document.contains("file") ||
      !document["file"].is_number_unsigned()) {
    return false;
  }
  identity->path = document["path"].get<std::string>();
  identity->size = document["size"].get<std::uintmax_t>();
  identity->modified = document["modified"].get<std::int64_t>();
  identity->device = document["device"].get<std::uint64_t>();
  identity->file = document["file"].get<std::uint64_t>();
  return !identity->path.empty();
}

bool same(const FileIdentity &left, const FileIdentity &right) {
  bool samePath = false;
#ifdef _WIN32
  const std::wstring leftPath = pathFromUtf8String(left.path).wstring();
  const std::wstring rightPath = pathFromUtf8String(right.path).wstring();
  samePath = CompareStringOrdinal(
                 leftPath.c_str(), static_cast<int>(leftPath.size()),
                 rightPath.c_str(), static_cast<int>(rightPath.size()), TRUE) ==
             CSTR_EQUAL;
#else
  samePath = left.path == right.path;
#endif
  const bool sameInstance =
      (left.device == 0 && left.file == 0 && right.device == 0 &&
       right.file == 0) ||
      (left.device == right.device && left.file == right.file);
  return samePath && sameInstance && left.size == right.size &&
         left.modified == right.modified;
}

bool samePath(const std::string &left, const std::string &right) {
#ifdef _WIN32
  const std::wstring leftPath = pathFromUtf8String(left).wstring();
  const std::wstring rightPath = pathFromUtf8String(right).wstring();
  return CompareStringOrdinal(
             leftPath.c_str(), static_cast<int>(leftPath.size()),
             rightPath.c_str(), static_cast<int>(rightPath.size()), TRUE) ==
         CSTR_EQUAL;
#else
  return left == right;
#endif
}

nlohmann::json encode(const ArtifactIdentity &identity) {
  return {{"path", identity.path},
          {"size", identity.size},
          {"sha256", identity.sha256}};
}

bool decode(const nlohmann::json &document, ArtifactIdentity *identity) {
  if (!identity || !document.is_object() || !document.contains("path") ||
      !document["path"].is_string() || !document.contains("size") ||
      !document["size"].is_number_unsigned() ||
      !document.contains("sha256") || !document["sha256"].is_string()) {
    return false;
  }
  identity->path = document["path"].get<std::string>();
  identity->size = document["size"].get<std::uintmax_t>();
  identity->sha256 = document["sha256"].get<std::string>();
  return !identity->path.empty() && identity->sha256.size() == 64;
}

bool artifactIdentityFor(const std::filesystem::path &contentPath,
                         const std::filesystem::path &identityPath,
                         ArtifactIdentity *identity, std::string *error) {
  if (!identity)
    return false;
  std::error_code pathError;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(identityPath, pathError);
  if (pathError) {
    pathError.clear();
    normalized = std::filesystem::absolute(identityPath, pathError)
                     .lexically_normal();
  }
  if (pathError || normalized.empty()) {
    setError(error, "Could not identify the transcript destination.");
    return false;
  }
  std::uintmax_t size = 0;
  std::string digest;
  if (!core_sha256::file(contentPath, {}, &size, &digest, error))
    return false;
  identity->path = toUtf8String(normalized);
  identity->size = size;
  identity->sha256 = std::move(digest);
  return true;
}

} // namespace

std::filesystem::path transcriptProvenancePath(
    const std::filesystem::path &transcriptPath) {
  if (transcriptPath.empty())
    return {};
  std::filesystem::path path = transcriptPath;
  path += L".radioify.json";
  return path;
}

std::optional<TranscriptSourceIdentity> captureTranscriptSourceIdentity(
    const std::filesystem::path &videoPath, std::string *error) {
  if (error)
    error->clear();
  FileIdentity video;
  if (!identityFor(videoPath, &video)) {
    setError(error, "Could not identify the source video.");
    return std::nullopt;
  }
  return TranscriptSourceIdentity{std::move(video.path), video.size,
                                  video.modified, video.device, video.file};
}

bool transcriptSourceMatches(const TranscriptSourceIdentity &expected,
                             const std::filesystem::path &videoPath) {
  FileIdentity current;
  if (!identityFor(videoPath, &current))
    return false;
  const FileIdentity recorded{expected.path, expected.size, expected.modified,
                              expected.device, expected.file};
  return same(recorded, current);
}

bool writeTranscriptProvenanceStaging(
    const TranscriptSourceIdentity &source,
    const std::string &producerIdentity,
    const std::filesystem::path &transcriptDestination,
    const std::filesystem::path &transcriptStaging,
    const std::filesystem::path &provenanceStaging, std::string *error) {
  if (error)
    error->clear();
  if (producerIdentity.empty()) {
    setError(error, "Transcript producer identity is empty.");
    return false;
  }
  ArtifactIdentity transcript;
  if (!artifactIdentityFor(transcriptStaging, transcriptDestination,
                           &transcript, error)) {
    return false;
  }
  std::ofstream output(provenanceStaging,
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create the transcript provenance record.");
    return false;
  }
  const FileIdentity video{source.path, source.size, source.modified,
                           source.device, source.file};
  const nlohmann::json document = {
      {"schema", kSchema},
      {"producer", producerIdentity},
      {"video", encode(video)},
      {"transcript", encode(transcript)}};
  output << document.dump(2) << '\n';
  output.flush();
  if (!output) {
    setError(error, "Could not finish the transcript provenance record.");
    return false;
  }
  output.close();
  return static_cast<bool>(output);
}

TranscriptProvenanceState verifyTranscriptProvenance(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &transcriptPath,
    const std::string &producerIdentity, std::string *error) {
  if (error)
    error->clear();
  const std::filesystem::path path =
      transcriptProvenancePath(transcriptPath);
  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    return TranscriptProvenanceState::Missing;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
  if (filesystemError || size == 0 || size > kMaximumManifestBytes) {
    setError(error, "The transcript provenance record is invalid.");
    return TranscriptProvenanceState::Invalid;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    nlohmann::json document;
    input >> document;
    FileIdentity recordedVideo;
    ArtifactIdentity recordedTranscript;
    if (!input || !document.is_object() ||
        document.value("schema", 0) != kSchema ||
        producerIdentity.empty() ||
        document.value("producer", std::string{}) != producerIdentity ||
        !document.contains("video") || !document.contains("transcript") ||
        !decode(document["video"], &recordedVideo) ||
        !decode(document["transcript"], &recordedTranscript)) {
      setError(error, "The transcript provenance record is invalid.");
      return TranscriptProvenanceState::Invalid;
    }
    FileIdentity currentVideo;
    ArtifactIdentity currentTranscript;
    std::string artifactError;
    if (!identityFor(videoPath, &currentVideo) ||
        !artifactIdentityFor(transcriptPath, transcriptPath,
                             &currentTranscript, &artifactError)) {
      if (error && !artifactError.empty())
        *error = std::move(artifactError);
      return TranscriptProvenanceState::Mismatch;
    }
    const bool sameTranscript =
        samePath(recordedTranscript.path, currentTranscript.path) &&
        recordedTranscript.size == currentTranscript.size &&
        recordedTranscript.sha256 == currentTranscript.sha256;
    return same(recordedVideo, currentVideo) && sameTranscript
               ? TranscriptProvenanceState::Matches
               : TranscriptProvenanceState::Mismatch;
  } catch (const nlohmann::json::exception &) {
    setError(error, "The transcript provenance record is invalid.");
    return TranscriptProvenanceState::Invalid;
  }
}

} // namespace playback_video_transcript
