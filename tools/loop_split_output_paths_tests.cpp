#include "audio/loopsplit/output_paths.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "core/runtime_helpers.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "loop_split_output_paths_tests: " << message << '\n';
  return false;
}

bool expectPaths(const LoopSplitOutputPaths& actual,
                 const std::filesystem::path& stinger,
                 const std::filesystem::path& loop,
                 const char* message) {
  return expect(actual.stinger == stinger && actual.loop == loop, message);
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path input = R"(C:\media\clip.mp4)";

  ok &= expectPaths(resolveLoopSplitOutputPaths(input, {}),
                    R"(C:\media\clip_stinger.wav)",
                    R"(C:\media\clip_loop.wav)",
                    "default outputs must stay beside the source");

  ok &= expectPaths(resolveLoopSplitOutputPaths(input, "named.flac"),
                    R"(C:\media\named_stinger.wav)",
                    R"(C:\media\named_loop.wav)",
                    "a relative filename must replace the output stem");

  ok &= expectPaths(
      resolveLoopSplitOutputPaths(input, R"(D:\exports\named.flac)"),
      R"(D:\exports\named_stinger.wav)",
      R"(D:\exports\named_loop.wav)",
      "an explicit filename must retain its requested directory");

  ok &= expectPaths(resolveLoopSplitOutputPaths(input, R"(D:\exports\)"),
                    R"(D:\exports\clip_stinger.wav)",
                    R"(D:\exports\clip_loop.wav)",
                    "a trailing separator must identify a target directory");

  std::error_code error;
  const std::filesystem::path existingDirectory =
      std::filesystem::current_path(error);
  ok &= expect(!error && !existingDirectory.empty(),
               "the test working directory must be available");
  if (!error && !existingDirectory.empty()) {
    ok &= expectPaths(
        resolveLoopSplitOutputPaths(input, toUtf8String(existingDirectory)),
        existingDirectory / "clip_stinger.wav",
        existingDirectory / "clip_loop.wav",
        "an existing directory must be recognized without a separator hint");
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
