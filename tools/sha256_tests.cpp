#include "core/sha256.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "sha256_tests: " << message << '\n';
  return false;
}

struct Vector {
  std::string input;
  const char* digest;
};

const std::array<Vector, 4> kVectors = {{
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {std::string(1'048'593, 'a'),
     "c26032d5154f96bd29c799447d715ab681d8d0aa308ecc6f321a35d98f0672da"},
}};

bool testTextLifetimes() {
  bool ok = true;
  for (const auto& vector : kVectors)
    ok &= expect(core_sha256::text(vector.input) == vector.digest,
                 "text hashing must match the known digest");

  // Each call constructs and destroys an independent native hash. Exercise
  // the same shared helper used concurrently by background media jobs.
  std::atomic<bool> matched{true};
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      for (size_t iteration = 0; iteration < 2000; ++iteration) {
        const auto& vector = kVectors[(iteration + worker) % 3];
        if (core_sha256::text(vector.input) != vector.digest)
          matched.store(false);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  return expect(matched.load(), "concurrent hash lifetimes must stay independent") && ok;
}

bool testFileLifetimes() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("radioify-sha256-tests-" + std::to_string(stamp));
  if (!expect(std::filesystem::create_directory(directory),
              "a private fixture directory must be created"))
    return false;

  bool ok = true;
  const auto path = directory / "input.bin";
  std::string error;
  std::string digest;
  std::uintmax_t size = 0;
  for (const auto& vector : kVectors) {
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(vector.input.data(),
                   static_cast<std::streamsize>(vector.input.size()));
      ok &= expect(static_cast<bool>(output), "the fixture must be written");
    }
    ok &= expect(core_sha256::file(path, {}, &size, &digest, &error) &&
                     digest == vector.digest && size == vector.input.size() &&
                     error.empty(),
                 "file hashing must match across empty, short and multi-chunk inputs");
  }

  for (int stopAfter : {1, 2}) {
    int checks = 0;
    digest = "previous result";
    ok &= expect(!core_sha256::file(path, [&] { return ++checks == stopAfter; },
                                  &size, &digest, &error) &&
                     checks == stopAfter && digest.empty() && !error.empty(),
                 "cancellation must destroy the live hash without publishing a digest");
  }
  ok &= expect(core_sha256::file(path, {}, &size, &digest, &error) &&
                   digest == kVectors.back().digest && error.empty(),
               "hashing must remain usable after cancellation");
  ok &= expect(!core_sha256::file(directory / "missing.bin", {}, &size, &digest,
                                &error) &&
                   digest.empty() && !error.empty(),
               "a missing input must not publish a digest");

  ok &= expect(std::filesystem::remove(path) && std::filesystem::remove(directory),
               "private fixtures must be removed after the test");
  return ok;
}

}  // namespace

int main() {
  const bool textOk = testTextLifetimes();
  const bool fileOk = testFileLifetimes();
  if (textOk && fileOk) std::cout << "sha256_tests: PASS\n";
  return textOk && fileOk ? 0 : 1;
}
