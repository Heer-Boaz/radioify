#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace core_sha256 {

bool file(const std::filesystem::path &path,
          const std::function<bool()> &cancelled, std::uintmax_t *size,
          std::string *digest, std::string *error = nullptr);

std::string text(const std::string &value);

} // namespace core_sha256
