#include "playback/video/analysis/scene_analysis_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include "core/runtime_helpers.h"

namespace playback_video_analysis {
namespace {

constexpr uint32_t kCacheSchemaVersion = 5;
constexpr const char* kCacheMagic = "RADIOIFY_SCENE_ANALYSIS";
constexpr size_t kMaximumCachedSuggestions = 10'000;

std::string cacheHeader() {
  return std::string(kCacheMagic) + "_V" +
         std::to_string(kCacheSchemaVersion);
}

std::string sha256Hex(const std::string& value) {
  AVSHA* context = av_sha_alloc();
  if (!context) return {};
  std::array<uint8_t, 32> digest{};
  const int initResult = av_sha_init(context, 256);
  if (initResult == 0) {
    av_sha_update(context, reinterpret_cast<const uint8_t*>(value.data()),
                  value.size());
    av_sha_final(context, digest.data());
  }
  av_free(context);
  if (initResult != 0) return {};

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

void appendFileIdentity(std::ostringstream* identity,
                        const std::filesystem::path& path) {
  if (!identity) return;
  if (path.empty()) {
    *identity << "none\n";
    return;
  }
  std::error_code ec;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(path, ec);
  if (ec || normalized.empty()) {
    ec.clear();
    normalized = std::filesystem::absolute(path, ec);
  }
  if (ec || normalized.empty()) normalized = path;
  ec.clear();
  const uintmax_t size = std::filesystem::file_size(path, ec);
  const uintmax_t stableSize = ec ? 0 : size;
  ec.clear();
  const auto modified = std::filesystem::last_write_time(path, ec);
  const int64_t modifiedTicks =
      ec ? 0 : static_cast<int64_t>(modified.time_since_epoch().count());
  *identity << toUtf8String(normalized) << '\n' << stableSize << '\n'
            << modifiedTicks << '\n';
}

std::filesystem::path cachePath(
    const std::filesystem::path& videoPath, int videoStreamIndex,
    int64_t durationUs, const std::filesystem::path& transcriptPath,
    bool createDirectory) {
  if (videoPath.empty() || durationUs <= 0) return {};
  std::ostringstream identity;
  identity << cacheHeader() << '\n' << videoStreamIndex << '\n' << durationUs
           << '\n' << kFeatureGridColumns << 'x' << kFeatureGridRows
           << "\nsample-us=" << kVisualSampleIntervalUs << '\n';
  appendFileIdentity(&identity, videoPath);
  appendFileIdentity(&identity, transcriptPath);
  const std::string hash = sha256Hex(identity.str());
  if (hash.empty()) return {};
  const std::filesystem::path directory =
      radioifyWritableDataDir() / "cache" /
      ("scene-analysis-v" + std::to_string(kCacheSchemaVersion));
  if (createDirectory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec || !std::filesystem::is_directory(directory, ec) || ec) return {};
  }
  return directory / (hash + ".txt");
}

bool finiteEvidence(const SceneSuggestion& suggestion) {
  return std::isfinite(suggestion.confidence) &&
         std::isfinite(suggestion.evidence.speechRatio) &&
         std::isfinite(suggestion.evidence.letterboxRatio) &&
         std::isfinite(suggestion.evidence.darkRatio) &&
         std::isfinite(suggestion.evidence.sceneChangeRate) &&
         std::isfinite(suggestion.evidence.visualMotion) &&
         std::isfinite(suggestion.evidence.hudLikelihood);
}

bool validSuggestions(const std::vector<SceneSuggestion>& suggestions,
                      int64_t durationUs) {
  if (suggestions.empty() ||
      suggestions.size() > kMaximumCachedSuggestions ||
      suggestions.front().startUs != 0 ||
      suggestions.back().endUs != durationUs) {
    return false;
  }
  int64_t previousEndUs = 0;
  uint64_t previousId = 0;
  for (const SceneSuggestion& suggestion : suggestions) {
    if (suggestion.id <= previousId || suggestion.startUs != previousEndUs ||
        suggestion.endUs <= suggestion.startUs ||
        suggestion.endUs > durationUs || suggestion.confidence < 0.0f ||
        suggestion.confidence > 1.0f || !finiteEvidence(suggestion)) {
      return false;
    }
    previousId = suggestion.id;
    previousEndUs = suggestion.endUs;
  }
  return true;
}

bool publishCacheFile(const std::filesystem::path& temporary,
                      const std::filesystem::path& destination) {
#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  std::filesystem::rename(temporary, destination, ec);
  return !ec;
#endif
}

}  // namespace

bool loadCachedSceneAnalysis(const std::filesystem::path& videoPath,
                             int videoStreamIndex, int64_t durationUs,
                             const std::filesystem::path& transcriptPath,
                             AnalysisResult* result) {
  if (!result) return false;
  const std::filesystem::path path =
      cachePath(videoPath, videoStreamIndex, durationUs, transcriptPath, false);
  if (path.empty()) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;

  std::string header;
  size_t sampleCount = 0;
  int usedTranscript = 0;
  size_t suggestionCount = 0;
  int64_t cachedDurationUs = 0;
  if (!std::getline(input, header) || header != cacheHeader() ||
      !(input >> cachedDurationUs >> sampleCount >> usedTranscript >>
        suggestionCount) ||
      cachedDurationUs != durationUs || suggestionCount == 0 ||
      suggestionCount > kMaximumCachedSuggestions ||
      (usedTranscript != 0 && usedTranscript != 1) ||
      (usedTranscript != 0 && transcriptPath.empty())) {
    return false;
  }

  std::vector<SceneSuggestion> suggestions;
  suggestions.reserve(suggestionCount);
  for (size_t index = 0; index < suggestionCount; ++index) {
    SceneSuggestion suggestion;
    int kind = 0;
    if (!(input >> suggestion.id >> suggestion.startUs >> suggestion.endUs >>
          kind >> suggestion.confidence >> suggestion.evidence.speechRatio >>
          suggestion.evidence.letterboxRatio >>
          suggestion.evidence.darkRatio >>
          suggestion.evidence.sceneChangeRate >>
          suggestion.evidence.visualMotion >>
          suggestion.evidence.hudLikelihood) ||
        kind < static_cast<int>(SceneKind::Gameplay) ||
        kind > static_cast<int>(SceneKind::MenuOrLoading)) {
      return false;
    }
    suggestion.kind = static_cast<SceneKind>(kind);
    suggestions.push_back(std::move(suggestion));
  }
  input >> std::ws;
  if (!input.eof() || !validSuggestions(suggestions, durationUs)) return false;

  result->durationUs = durationUs;
  result->visualSampleCount = sampleCount;
  result->transcriptPath = usedTranscript != 0 ? transcriptPath
                                               : std::filesystem::path{};
  result->suggestions = std::move(suggestions);
  return true;
}

void storeCachedSceneAnalysis(const std::filesystem::path& videoPath,
                              int videoStreamIndex, int64_t durationUs,
                              const std::filesystem::path& transcriptPath,
                              const AnalysisResult& result) {
  if (result.durationUs != durationUs ||
      !validSuggestions(result.suggestions, durationUs)) {
    return;
  }
  const std::filesystem::path path =
      cachePath(videoPath, videoStreamIndex, durationUs, transcriptPath, true);
  if (path.empty()) return;
  std::filesystem::path temporary = path;
  temporary += ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" +
               std::to_string(GetCurrentThreadId());

  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << cacheHeader() << '\n' << durationUs << ' '
           << result.visualSampleCount << ' '
           << (!result.transcriptPath.empty() ? 1 : 0) << ' '
           << result.suggestions.size() << '\n'
           << std::setprecision(9);
    for (const SceneSuggestion& suggestion : result.suggestions) {
      output << suggestion.id << ' ' << suggestion.startUs << ' '
             << suggestion.endUs << ' '
             << static_cast<int>(suggestion.kind) << ' '
             << suggestion.confidence << ' '
             << suggestion.evidence.speechRatio << ' '
             << suggestion.evidence.letterboxRatio << ' '
             << suggestion.evidence.darkRatio << ' '
             << suggestion.evidence.sceneChangeRate << ' '
             << suggestion.evidence.visualMotion << ' '
             << suggestion.evidence.hudLikelihood << '\n';
    }
    output.flush();
    if (!output) {
      output.close();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return;
    }
  }
  if (!publishCacheFile(temporary, path)) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  }
}

}  // namespace playback_video_analysis
