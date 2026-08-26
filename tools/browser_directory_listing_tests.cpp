#include "tui/ui/browser_directory_listing.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "browser_directory_listing_tests: " << message << '\n';
  return false;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("radioify-browser-directory-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::create_directories(path_, error);
    valid_ = !error;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  bool valid() const { return valid_; }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  bool valid_ = false;
};

bool writeFile(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  return output.good();
}

const BrowserEntry* findEntry(const std::vector<BrowserEntry>& entries,
                              const std::string& name) {
  for (const BrowserEntry& entry : entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;
  TemporaryDirectory fixture;
  if (!fixture.valid()) {
    std::cerr << "browser_directory_listing_tests: could not create fixture\n";
    return EXIT_FAILURE;
  }

  std::error_code error;
  const std::filesystem::path folder = fixture.path() / "folder";
  std::filesystem::create_directory(folder, error);
  if (error || !writeFile(fixture.path() / "clip.mp4", "video") ||
      !writeFile(fixture.path() / "cover.png", "art") ||
      !writeFile(fixture.path() / "notes.txt", "ignore")) {
    std::cerr << "browser_directory_listing_tests: could not prepare fixture\n";
    return EXIT_FAILURE;
  }

  browser_directory_listing::Result result =
      browser_directory_listing::list(fixture.path());
  const auto* entries = std::get_if<std::vector<BrowserEntry>>(&result);
  ok &= expect(entries != nullptr,
               "a readable directory must produce browser entries");
  if (entries) {
    const BrowserEntry* parent = findEntry(*entries, "..");
    const BrowserEntry* directory = findEntry(*entries, "folder");
    const BrowserEntry* video = findEntry(*entries, "clip.mp4");
    const BrowserEntry* artwork = findEntry(*entries, "cover.png");
    ok &= expect(parent &&
                     parent->actionAs<browser_entry::NavigateUp>() != nullptr,
                 "a non-root directory must include parent navigation");
    ok &= expect(directory &&
                     directory->actionAs<browser_entry::OpenDirectory>() !=
                         nullptr,
                 "child directories must be listed as navigation entries");
    ok &= expect(video && video->actionAs<browser_entry::OpenFile>() != nullptr,
                 "supported videos must be listed as media entries");
    ok &= expect(video && video->sortMetadata.size == 5,
                 "media entries must retain file-size sort metadata");
    ok &= expect(artwork != nullptr,
                 "visible image files must remain browsable media");
    ok &= expect(findEntry(*entries, "notes.txt") == nullptr,
                 "unsupported files must not leak into the media browser");
  }

  result = browser_directory_listing::list(fixture.path(), [] { return true; });
  ok &= expect(
      std::holds_alternative<browser_directory_listing::Cancelled>(result),
      "a cancelled request must not publish a partial directory listing");

  result = browser_directory_listing::list(fixture.path() / "missing");
  const auto* listingError =
      std::get_if<browser_directory_listing::Error>(&result);
  ok &= expect(listingError &&
                   listingError->message.find("Unable to open this folder:") ==
                       0,
               "filesystem failures must retain a user-facing error");

#ifdef _WIN32
  const std::filesystem::path artworkPath = fixture.path() / "cover.png";
  const DWORD originalAttributes = GetFileAttributesW(artworkPath.c_str());
  if (originalAttributes == INVALID_FILE_ATTRIBUTES ||
      !SetFileAttributesW(artworkPath.c_str(),
                          originalAttributes | FILE_ATTRIBUTE_HIDDEN)) {
    std::cerr << "browser_directory_listing_tests: could not hide artwork\n";
    return EXIT_FAILURE;
  }
  result = browser_directory_listing::list(fixture.path());
  entries = std::get_if<std::vector<BrowserEntry>>(&result);
  ok &= expect(entries && findEntry(*entries, "cover.png") == nullptr,
               "hidden artwork sidecars must stay out of the media browser");
  SetFileAttributesW(artworkPath.c_str(), originalAttributes);
#endif

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
