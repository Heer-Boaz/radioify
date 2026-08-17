#include <iostream>
#include <string>

#include "browser_navigation.h"
#include "playback_target_match.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "browser_navigation_tests: " << message << '\n';
    return false;
  }
  return true;
}

FileEntry fileEntry(const std::string& name, const std::filesystem::path& path,
                    bool isDir = false, int trackIndex = -1) {
  FileEntry entry;
  entry.name = name;
  entry.path = path;
  entry.isDir = isDir;
  entry.trackIndex = trackIndex;
  return entry;
}

GridLayout verticalLayout(int totalRows, int visibleRows) {
  GridLayout layout;
  layout.totalRows = totalRows;
  layout.rowsVisible = visibleRows;
  layout.cols = 1;
  return layout;
}

}  // namespace

int main() {
  bool ok = true;

  const std::filesystem::path songA = "C:/Media/A.flac";
  const std::filesystem::path songB = "C:/Media/B.flac";
  std::vector<FileEntry> files{fileEntry("A.flac", songA),
                               fileEntry("B.flac", songB)};
  const PlaybackTarget playingA{songA, -1};
  ok &= expect(findBrowserPlaybackTargetEntry(files, playingA) == 0,
               "the playing item must be found independently of selection");
  ok &= expect(
      browserEntryMatchesPlaybackTarget(files[0], {"C:/Media/./A.flac", -1}),
      "lexically equivalent playback paths must match");

  const PlaybackTarget playingContainerTrack{songA, 3};
  ok &=
      expect(browserEntryMatchesPlaybackTarget(files[0], playingContainerTrack),
             "a container file must remain active for an internal track");
  std::vector<FileEntry> tracks{fileEntry("Track 1", songA, false, 0),
                                fileEntry("Track 4", songA, false, 3)};
  ok &=
      expect(findBrowserPlaybackTargetEntry(tracks, playingContainerTrack) == 1,
             "a track browser must mark only the exact playing track");
  ok &= expect(!browserEntryMatchesPlaybackTarget(
                   fileEntry("Media", "C:/Media", true), playingA),
               "directories must never receive a playback marker");

  BrowserState browser;
  browser.viewMode = BrowserState::ViewMode::Thumbnails;
  browser.dir = "C:/Media";
  for (int i = 0; i < 30; ++i) {
    browser.entries.push_back(
        fileEntry("Folder " + std::to_string(i),
                  browser.dir / ("Folder " + std::to_string(i)), true));
  }
  browser.selected = 17;
  browser.scrollRow = 13;
  const BrowserState::Location saved =
      captureBrowserLocation(browser, BrowserState::LocationKind::Directory);

  browser.selected = 0;
  browser.scrollRow = 0;
  ok &= expect(restoreBrowserLocation(browser, saved),
               "history must restore the selected entry by identity");
  applyBrowserViewportRestore(browser, verticalLayout(30, 5));
  ok &= expect(browser.selected == 17 && browser.scrollRow == 13,
               "history must restore an unchanged selection and viewport");

  const FileEntry moved = browser.entries[17];
  browser.entries.erase(browser.entries.begin() + 17);
  browser.entries.insert(browser.entries.begin() + 23, moved);
  browser.selected = 0;
  browser.scrollRow = 0;
  ok &= expect(restoreBrowserLocation(browser, saved),
               "history must survive entry reordering");
  applyBrowserViewportRestore(browser, verticalLayout(30, 5));
  ok &= expect(browser.selected == 23 && browser.scrollRow == 19,
               "a moved restored entry must be scrolled back into view");

  const BrowserState::EntryIdentity departed =
      browserEntryIdentity(browser.entries[23]);
  browser.selected = 0;
  browser.scrollRow = 0;
  ok &= expect(selectBrowserEntry(browser, departed),
               "Up navigation must be able to refocus the departed folder");
  requestBrowserSelectionReveal(browser);
  applyBrowserViewportRestore(browser, verticalLayout(30, 5));
  ok &= expect(browser.selected == 23 && browser.scrollRow == 19,
               "the departed folder must be visible after Up navigation");

  browser.viewMode = BrowserState::ViewMode::ListOnly;
  browser.selected = 23;
  browser.scrollRow = 0;
  GridLayout multiColumnList = verticalLayout(25, 5);
  multiColumnList.cols = 2;
  requestBrowserSelectionReveal(browser);
  applyBrowserViewportRestore(browser, multiColumnList);
  ok &= expect(browser.scrollRow == 14,
               "list columns must reveal the selected entry as one viewport");

  BrowserState historyBrowser;
  historyBrowser.dir = "C:/Media";
  historyBrowser.entries = files;
  historyBrowser.selected = 1;
  historyBrowser.scrollRow = 4;
  const BrowserState::Location parentLocation = captureBrowserLocation(
      historyBrowser, BrowserState::LocationKind::Directory);

  historyBrowser.dir = songA;
  historyBrowser.entries = tracks;
  historyBrowser.selected = 0;
  historyBrowser.scrollRow = 0;
  BrowserState::Location trackBrowserLocation = captureBrowserLocation(
      historyBrowser, BrowserState::LocationKind::TrackBrowser);
  ok &= expect(recordBrowserNavigation(historyBrowser, parentLocation,
                                       trackBrowserLocation),
               "a real location change must create one history entry");
  ok &= expect(historyBrowser.backHistory.size() == 1 &&
                   historyBrowser.forwardHistory.empty(),
               "new navigation must own the Back stack and clear Forward");

  historyBrowser.selected = 1;
  historyBrowser.scrollRow = 6;
  BrowserState::Location currentTrackLocation = captureBrowserLocation(
      historyBrowser, BrowserState::LocationKind::TrackBrowser);
  const std::optional<BrowserState::Location> backTarget =
      browserHistoryBack(historyBrowser, currentTrackLocation);
  ok &= expect(backTarget && backTarget->dir == parentLocation.dir &&
                   backTarget->selectedEntry &&
                   backTarget->selectedEntry->path == songB &&
                   backTarget->scrollRow == 4,
               "Back must return the exact source location");

  historyBrowser.dir = parentLocation.dir;
  historyBrowser.entries = files;
  historyBrowser.selected = 1;
  historyBrowser.scrollRow = 4;
  const std::optional<BrowserState::Location> forwardTarget =
      browserHistoryForward(
          historyBrowser,
          captureBrowserLocation(historyBrowser,
                                 BrowserState::LocationKind::Directory));
  ok &= expect(
      forwardTarget &&
          forwardTarget->kind == BrowserState::LocationKind::TrackBrowser &&
          forwardTarget->dir == songA && forwardTarget->selectedEntry &&
          forwardTarget->selectedEntry->trackIndex == 3 &&
          forwardTarget->scrollRow == 6,
      "Forward must restore the destination as it was left");
  if (forwardTarget) {
    ok &=
        expect(!recordBrowserNavigation(historyBrowser, *forwardTarget,
                                        *forwardTarget),
               "playback inside one location must not pollute browser history");
  }

  BrowserState locationKindBrowser;
  BrowserState::Location physicalLocation = trackBrowserLocation;
  physicalLocation.kind = BrowserState::LocationKind::Directory;
  ok &= expect(recordBrowserNavigation(locationKindBrowser, physicalLocation,
                                       trackBrowserLocation),
               "physical and virtual locations with one path must differ");

#ifdef _WIN32
  const std::optional<std::filesystem::path> driveParent =
      browserParentDirectory(std::filesystem::path("C:\\"));
  ok &= expect(driveParent.has_value() && driveParent->empty(),
               "a drive root must navigate up to the virtual drive list");
#endif

  return ok ? 0 : 1;
}
