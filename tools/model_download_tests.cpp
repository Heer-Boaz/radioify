#include "core/model_download.h"
#include "core/sha256.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void write(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << contents;
  require(static_cast<bool>(stream), "Could not write fixture");
}
} // namespace

int main() {
  namespace fs = std::filesystem;
  using namespace model_download;
  const auto root =
      fs::temp_directory_path() /
      ("radioify-model-download-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  int exitCode = 0;
  try {
    const std::string contents = "A generic model download fixture.";
    const auto hash = core_sha256::text(contents);
    // Every successful case must finish without contacting this invalid host.
    const Artifact artifact{L"unused.invalid", L"/model", "Downloading fixture",
                            contents.size(), hash.c_str()};
    const auto destination = root / "model.bin";
    const auto partial = root / "model.bin.partial";
    require(!verified(destination, artifact), "Missing model accepted");

    write(partial, contents);
    auto result = ensure(destination, artifact);
    require(result.status == Status::Succeeded,
            "Complete partial did not publish");
    require(fs::exists(destination) && !fs::exists(partial),
            "Publication left a partial or lost the model");
    require(ensure(destination, artifact).status == Status::Succeeded,
            "Verified cache did not work offline");

    const auto timestamp = fs::last_write_time(destination);
    write(destination, std::string(contents.size(), 'x'));
    fs::last_write_time(destination, timestamp + std::chrono::seconds(2));
    require(!verified(destination, artifact), "Corrupted cache accepted");

    write(partial, contents.substr(0, 8));
    const Control cancelled{[] { return true; }, {}};
    result = ensure(destination, artifact, cancelled);
    require(result.status == Status::Cancelled && fs::file_size(partial) == 8,
            "Cancellation removed the resumable partial");
    require(fs::file_size(destination) == contents.size(),
            "Cancellation changed the published file");

    write(partial, contents);
    result = ensure(destination, artifact);
    require(result.status == Status::Succeeded &&
                verified(destination, artifact),
            "Valid partial did not repair a corrupted cache");
    require(fs::exists(root / "model.bin.verified"), "No verification receipt");
    std::cout << "Model cache, verified publication, repair and cancellation "
                 "passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    exitCode = 1;
  }
  fs::remove_all(root);
  return exitCode;
}
