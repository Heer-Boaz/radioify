#include "playback/video/chapter/helper_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <sstream>

namespace playback_video_chapters {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string trim(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(),
              value.end());
  return value;
}

bool isVulkanDeviceId(const std::string& token) {
  const std::string normalized = lower(token);
  constexpr std::string_view kPrefix = "vulkan";
  return normalized.size() > kPrefix.size() &&
         std::string_view(normalized).substr(0, kPrefix.size()) == kPrefix &&
         std::all_of(normalized.begin() +
                         static_cast<std::ptrdiff_t>(kPrefix.size()),
                     normalized.end(), [](unsigned char ch) {
                       return std::isdigit(ch) != 0;
                     });
}

std::uint64_t freeMemoryMiB(const std::string& row) {
  constexpr std::string_view kSuffix = " MiB free)";
  const std::size_t suffix = row.rfind(kSuffix);
  if (suffix == std::string::npos || suffix == 0) return 0;
  std::size_t begin = suffix;
  while (begin > 0 &&
         std::isdigit(static_cast<unsigned char>(row[begin - 1])) != 0) {
    --begin;
  }
  if (begin == suffix) return 0;
  std::uint64_t value = 0;
  const auto parsed =
      std::from_chars(row.data() + begin, row.data() + suffix, value);
  return parsed.ec == std::errc{} && parsed.ptr == row.data() + suffix
             ? value
             : 0;
}

}  // namespace

std::optional<std::string> parseVulkanDeviceList(std::string_view output) {
  std::istringstream lines{std::string(output)};
  std::string line;
  bool inDeviceTable = false;
  std::optional<std::string> selected;
  std::uint64_t selectedFreeMemoryMiB = 0;
  while (std::getline(lines, line)) {
    const std::string cleaned = trim(line);
    if (!inDeviceTable) {
      inDeviceTable = lower(cleaned) == "available devices:";
      continue;
    }
    const std::size_t colon = cleaned.find(':');
    if (colon == std::string::npos) continue;
    const std::string token = trim(cleaned.substr(0, colon));
    if (!isVulkanDeviceId(token)) continue;
    const std::uint64_t freeMiB = freeMemoryMiB(cleaned);
    if (!selected || freeMiB > selectedFreeMemoryMiB) {
      selected = token;
      selectedFreeMemoryMiB = freeMiB;
    }
  }
  return selected;
}

bool confirmsGpuOnlyInference(std::string_view output) {
  const std::string log = lower(std::string(output));
  if (log.find("clip using cpu backend") != std::string::npos ||
      log.find("clip using vulkan") == std::string::npos) {
    return false;
  }
  for (std::size_t position = log.find("offloaded ");
       position != std::string::npos;
       position = log.find("offloaded ", position + 1)) {
    int offloaded = 0;
    int total = 0;
    const char* begin = log.data() + position + 10;
    const char* end = log.data() + log.size();
    const auto first = std::from_chars(begin, end, offloaded);
    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != '/') {
      continue;
    }
    const auto second = std::from_chars(first.ptr + 1, end, total);
    constexpr std::string_view kSuffix = " layers to gpu";
    if (second.ec == std::errc{} &&
        static_cast<std::size_t>(end - second.ptr) >= kSuffix.size() &&
        std::string_view(second.ptr, kSuffix.size()) == kSuffix && total > 0 &&
        offloaded == total) {
      return true;
    }
  }
  return false;
}

}  // namespace playback_video_chapters
