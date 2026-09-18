#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace playback_video_analysis {

bool sha256File(const std::filesystem::path &path,
                const std::function<bool()> &cancelled, std::uintmax_t *size,
                std::string *digest, std::string *error = nullptr);

std::string sha256Text(const std::string &value);

} // namespace playback_video_analysis
