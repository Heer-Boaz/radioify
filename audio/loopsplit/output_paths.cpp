#include "audio/loopsplit/output_paths.h"

#include <system_error>

#include "core/runtime_helpers.h"

LoopSplitOutputPaths resolveLoopSplitOutputPaths(
    const std::filesystem::path& input, const std::string& outputArgument) {
  const std::string base = toUtf8String(input.stem());
  constexpr const char* kStingerSuffix = "_stinger.wav";
  constexpr const char* kLoopSuffix = "_loop.wav";

  if (outputArgument.empty()) {
    return {input.parent_path() / (base + kStingerSuffix),
            input.parent_path() / (base + kLoopSuffix)};
  }

  const std::filesystem::path outputPath =
      pathFromUtf8String(outputArgument);
  const bool directoryHint = outputArgument.back() == '/' ||
                             outputArgument.back() == '\\';
  std::error_code error;
  const bool existingDirectory =
      std::filesystem::is_directory(outputPath, error);
  if (directoryHint || (!error && existingDirectory)) {
    return {outputPath / (base + kStingerSuffix),
            outputPath / (base + kLoopSuffix)};
  }

  std::filesystem::path outputDirectory = outputPath.parent_path();
  if (outputDirectory.empty()) outputDirectory = input.parent_path();
  std::string outputStem = toUtf8String(outputPath.stem());
  if (outputStem.empty()) outputStem = base;
  return {outputDirectory / (outputStem + kStingerSuffix),
          outputDirectory / (outputStem + kLoopSuffix)};
}
