#include "playback/video/chapter/cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/path_identity.h"
#include "core/runtime_helpers.h"
#include "playback/video/chapter/integrity.h"
#include "playback/video/chapter/model.h"

namespace playback_video_chapters {
namespace {

constexpr int kSchema = 1;

void setError(std::string* error, std::string value) {
  if (error) *error = std::move(value);
}

std::string sourceIdentity(const AnalysisRequest& request) {
  std::error_code error;
  const PathIdentity path = makePathIdentity(request.file);
  const std::uintmax_t size =
      std::filesystem::file_size(request.file, error);
  const std::uintmax_t stableSize = error ? 0 : size;
  error.clear();
  const auto modified = std::filesystem::last_write_time(request.file, error);
  const std::int64_t ticks =
      error ? 0
            : static_cast<std::int64_t>(modified.time_since_epoch().count());
  std::ostringstream identity;
  identity << "radioify-video-chapters-v" << kSchema << '\n'
           << toUtf8String(path.normalizedPath) << '\n'
           << stableSize << '\n'
           << ticks << '\n'
           << request.videoStreamIndex << '\n'
           << request.durationUs << '\n'
           << request.sourceWidth << 'x' << request.sourceHeight << '\n'
           << kModelSha256 << '\n'
           << kProjectorSha256 << '\n';
  if (request.englishText) identity << request.englishText->identity;
  return identity.str();
}

std::filesystem::path cachePath(const AnalysisRequest& request) {
  const std::string hash = sha256Text(sourceIdentity(request));
  if (hash.empty()) return {};
  return radioifyWritableDataDir() / "cache" / "video-chapters" /
         (hash + ".json");
}

bool decode(const nlohmann::json& document, std::int64_t durationUs,
            AnalysisResult* result) {
  if (!result || !document.is_object() ||
      document.value("schema", 0) != kSchema ||
      document.value("duration_us", std::int64_t{0}) != durationUs ||
      !document.contains("chapters") ||
      !document["chapters"].is_array()) {
    return false;
  }
  AnalysisResult decoded;
  decoded.status = OperationStatus::Succeeded;
  decoded.overview = document.value("overview", std::string{});
  for (const nlohmann::json& item : document["chapters"]) {
    if (!item.is_object()) return false;
    Chapter chapter;
    chapter.id = item.value("id", std::uint64_t{0});
    chapter.startUs = item.value("start_us", std::int64_t{-1});
    chapter.endUs = item.value("end_us", std::int64_t{-1});
    chapter.title = item.value("title", std::string{});
    chapter.summary = item.value("summary", std::string{});
    decoded.chapters.push_back(std::move(chapter));
  }
  if (!validatePartition(durationUs, decoded.chapters)) return false;
  *result = std::move(decoded);
  return true;
}

}  // namespace

std::optional<AnalysisResult> loadCachedAnalysis(
    const AnalysisRequest& request) {
  const std::filesystem::path path = cachePath(request);
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  try {
    nlohmann::json document;
    input >> document;
    AnalysisResult result;
    if (!input || !decode(document, request.durationUs, &result)) {
      return std::nullopt;
    }
    return result;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool storeCachedAnalysis(const AnalysisRequest& request,
                         const AnalysisResult& result,
                         std::string* error) {
  if (error) error->clear();
  if (result.status != OperationStatus::Succeeded ||
      !validatePartition(request.durationUs, result.chapters, error)) {
    return false;
  }
  const std::filesystem::path path = cachePath(request);
  if (path.empty()) {
    setError(error, "Could not create a chapter cache identity.");
    return false;
  }
  std::error_code filesystemError;
  std::filesystem::create_directories(path.parent_path(), filesystemError);
  if (filesystemError) {
    setError(error, "Could not create the chapter cache directory.");
    return false;
  }
  std::filesystem::path staging = path;
  staging += L".partial-" + std::to_wstring(GetCurrentProcessId());

  nlohmann::json chapters = nlohmann::json::array();
  for (const Chapter& chapter : result.chapters) {
    chapters.push_back({{"id", chapter.id},
                        {"start_us", chapter.startUs},
                        {"end_us", chapter.endUs},
                        {"title", chapter.title},
                        {"summary", chapter.summary}});
  }
  const nlohmann::json document = {
      {"schema", kSchema},
      {"duration_us", request.durationUs},
      {"overview", result.overview},
      {"chapters", std::move(chapters)}};
  std::ofstream output(staging, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create the chapter cache staging file.");
    return false;
  }
  output << document.dump(2) << '\n';
  output.flush();
  if (!output) {
    setError(error, "Could not finish the chapter cache staging file.");
    return false;
  }
  output.close();
  if (!MoveFileExW(staging.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    setError(error, "Could not publish the chapter cache.");
    return false;
  }
  return true;
}

}  // namespace playback_video_chapters
