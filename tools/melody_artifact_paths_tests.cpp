#include "audio/analysis/melody_artifact_paths.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "core/runtime_helpers.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "melody_artifact_paths_tests: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path input = R"(C:\media\archive.minipsf)";

  ok &= expect(defaultMelodyArtifactPath(input) ==
                   std::filesystem::path(R"(C:\media\archive.melody)"),
               "the default artifact must replace the source extension");
  ok &= expect(melodyArtifactPathForTrack(input, 0) ==
                   std::filesystem::path(
                       R"(C:\media\archive.minipsf.track000.melody)"),
               "track zero must retain the source extension and use padding");
  ok &= expect(melodyArtifactPathForTrack(input, 7) ==
                   std::filesystem::path(
                       R"(C:\media\archive.minipsf.track007.melody)"),
               "single-digit tracks must have stable three-digit names");
  ok &= expect(melodyArtifactPathForTrack(input, 1234) ==
                   std::filesystem::path(
                       R"(C:\media\archive.minipsf.track1234.melody)"),
               "large track indices must not be truncated");

  ok &= expect(resolveMelodyArtifactPath(input, R"(D:\exports\named)") ==
                   std::filesystem::path(R"(D:\exports\named.melody)"),
               "an extensionless output must receive the artifact extension");
  ok &= expect(
      resolveMelodyArtifactPath(input, R"(D:\exports\named.custom)") ==
          std::filesystem::path(R"(D:\exports\named.custom)"),
      "an explicit output extension must be preserved");
  ok &= expect(resolveMelodyArtifactPath(input, R"(D:\exports\)") ==
                   std::filesystem::path(
                       R"(D:\exports\archive.melody)"),
               "a separator hint must select a target directory");

  std::error_code error;
  const std::filesystem::path existingDirectory =
      std::filesystem::current_path(error);
  ok &= expect(!error && !existingDirectory.empty(),
               "the test working directory must be available");
  if (!error && !existingDirectory.empty()) {
    ok &= expect(
        resolveMelodyArtifactPath(input, toUtf8String(existingDirectory)) ==
            existingDirectory / "archive.melody",
        "an existing directory must be recognized without a separator hint");
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
