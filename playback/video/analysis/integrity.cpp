#include "playback/video/analysis/integrity.h"

#include "core/sha256.h"

namespace playback_video_analysis {

bool sha256File(const std::filesystem::path &path,
                const std::function<bool()> &cancelled, std::uintmax_t *size,
                std::string *digest, std::string *error) {
  return core_sha256::file(path, cancelled, size, digest, error);
}

std::string sha256Text(const std::string &value) {
  return core_sha256::text(value);
}

} // namespace playback_video_analysis
