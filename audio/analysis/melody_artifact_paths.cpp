#include "audio/analysis/melody_artifact_paths.h"

#include <system_error>

#include "core/runtime_helpers.h"

std::filesystem::path defaultMelodyArtifactPath(
    const std::filesystem::path& input) {
  std::filesystem::path output = input;
  output.replace_extension(".melody");
  return output;
}

std::filesystem::path melodyArtifactPathForTrack(
    const std::filesystem::path& input, std::uint32_t trackIndex) {
  std::string index = std::to_string(trackIndex);
  if (index.size() < 3) index.insert(0, 3 - index.size(), '0');
  std::filesystem::path output = input;
  output += ".track" + index + ".melody";
  return output;
}

std::filesystem::path resolveMelodyArtifactPath(
    const std::filesystem::path& input, const std::string& outputArgument) {
  if (outputArgument.empty()) return defaultMelodyArtifactPath(input);

  std::filesystem::path output = pathFromUtf8String(outputArgument);
  const bool directoryHint = outputArgument.back() == '/' ||
                             outputArgument.back() == '\\';
  std::error_code error;
  const bool existingDirectory = std::filesystem::is_directory(output, error);
  if (directoryHint || (!error && existingDirectory)) {
    return output / defaultMelodyArtifactPath(input).filename();
  }
  if (!output.has_extension()) output += ".melody";
  return output;
}
