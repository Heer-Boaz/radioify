#pragma once

#include <filesystem>

struct PathIdentity {
  std::filesystem::path normalizedPath;

  bool empty() const { return normalizedPath.empty(); }
};

PathIdentity makePathIdentity(const std::filesystem::path& path);
bool samePathIdentity(const PathIdentity& left, const PathIdentity& right);
// Stable textual key for hashing or persistence. On Windows this applies the
// same case-insensitive identity semantics used by samePathIdentity().
std::filesystem::path pathIdentityKey(const PathIdentity& identity);
bool samePath(const std::filesystem::path& left,
              const std::filesystem::path& right);

inline bool operator==(const PathIdentity& left, const PathIdentity& right) {
  return samePathIdentity(left, right);
}

inline bool operator!=(const PathIdentity& left, const PathIdentity& right) {
  return !samePathIdentity(left, right);
}
