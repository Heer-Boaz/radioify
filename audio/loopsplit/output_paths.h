#pragma once

#include <filesystem>
#include <string>

struct LoopSplitOutputPaths {
  std::filesystem::path stinger;
  std::filesystem::path loop;
};

LoopSplitOutputPaths resolveLoopSplitOutputPaths(
    const std::filesystem::path& input, const std::string& outputArgument);
