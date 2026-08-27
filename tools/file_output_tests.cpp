#include "core/file_output.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "file_output_tests: " << message << '\n';
  return false;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::string& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
}

}  // namespace

int main() {
  namespace output = file_output;
  bool ok = true;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-file-output-tests-" + std::to_string(stamp));
  std::filesystem::create_directories(directory);

  const std::filesystem::path source = directory / "movie.mp4";
  const std::filesystem::path first =
      output::uniqueSiblingPath(source, L" - audio", L".flac");
  writeFile(first, "occupied");
  const std::filesystem::path second =
      output::uniqueSiblingPath(source, L" - audio", L".flac");
  ok &= expect(first.filename() == "movie - audio.flac" &&
                   second.filename() == "movie - audio (2).flac",
               "sibling export names must be readable and non-destructive");

  std::string error;
  auto create = output::Transaction::begin(
      second, output::PublishMode::CreateNew, &error);
  const std::filesystem::path createTemporary =
      create ? create->temporaryPath() : std::filesystem::path{};
  if (create) writeFile(create->temporaryPath(), "new");
  ok &= expect(create && create->publish(&error) &&
                   readFile(second) == "new" &&
                   !std::filesystem::exists(createTemporary),
               "CreateNew must publish one completed file");

  auto rejected = output::Transaction::begin(
      second, output::PublishMode::CreateNew, &error);
  ok &= expect(!rejected && readFile(second) == "new",
               "CreateNew must reject an existing destination");

  auto replacement = output::Transaction::begin(
      second, output::PublishMode::ReplaceExisting, &error);
  if (replacement) writeFile(replacement->temporaryPath(), "replacement");
  ok &= expect(replacement && replacement->publish(&error) &&
                   readFile(second) == "replacement",
               "ReplaceExisting must atomically replace completed output");

  std::filesystem::path abandonedTemporary;
  {
    auto abandoned = output::Transaction::begin(
        directory / "abandoned.txt", output::PublishMode::CreateNew,
        &error);
    if (abandoned) {
      abandonedTemporary = abandoned->temporaryPath();
      writeFile(abandonedTemporary, "partial");
    }
  }
  ok &= expect(!abandonedTemporary.empty() &&
                   !std::filesystem::exists(abandonedTemporary) &&
                   !std::filesystem::exists(directory / "abandoned.txt"),
               "an unpublished transaction must clean up partial output");

  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
