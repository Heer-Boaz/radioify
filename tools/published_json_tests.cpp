#include "core/published_json.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

int main() {
  namespace fs = std::filesystem;
  const auto root =
      fs::temp_directory_path() /
      ("radioify-json-publication-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  const auto path = root / "progress.json";
  const auto pending = root / "pending.json";
  const std::string contents =
      nlohmann::json{{"sequence", 1}, {"padding", std::string(128 * 1024, 'x')}}
          .dump();
  const auto write = [&](const fs::path &file) {
    std::ofstream output(file, std::ios::binary);
    output << contents;
    output.close();
    return static_cast<bool>(output);
  };
  bool ok = write(path) && readPublishedJson(path, 64).is_null();
  std::atomic<bool> finished{false};
  std::atomic<unsigned> validReads{0};
  std::atomic<bool> invalidRead{false};
  std::thread reader([&] {
    do {
      const auto value = readPublishedJson(path, 256 * 1024);
      if (value.is_object() && value.value("sequence", 0) == 1)
        ++validReads;
      else if (!value.is_null())
        invalidRead = true;
    } while (!finished);
  });
  DWORD publishError = 0;
  for (int index = 0; index < 200; ++index) {
    if (!write(pending)) {
      ok = false;
      break;
    }
    // Match the producer's bounded retry: even delete-sharing readers can
    // briefly overlap Windows' completion of a previous file replacement.
    bool published = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        published = true;
        break;
      }
      publishError = GetLastError();
      if (publishError != ERROR_ACCESS_DENIED &&
          publishError != ERROR_SHARING_VIOLATION) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!published) { ok = false; break; }
  }
  finished = true;
  reader.join();
  ok = ok && validReads > 0 && !invalidRead;
  fs::remove_all(root);
  if (!ok) {
    std::cerr
        << "JSON readers blocked atomic publication or read invalid data; "
        << "Windows error " << publishError << '\n';
    return 1;
  }
  std::cout
      << "Bounded JSON reads and 200 concurrent atomic publications passed.\n";
  return 0;
}
