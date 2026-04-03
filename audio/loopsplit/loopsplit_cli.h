#ifndef LOOPSPLIT_CLI_H
#define LOOPSPLIT_CLI_H

#include <filesystem>
#include <utility>
#include <string>

#include "app_common.h"

int runSplitLoopCli(const Options& o);

std::pair<std::filesystem::path, std::filesystem::path> resolveSplitOutputPaths(
    const std::filesystem::path& input, const std::string& outArg);

#endif  // LOOPSPLIT_CLI_H
