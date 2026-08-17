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
  FileEntry entry{name, path, isDir};
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
#ifdef _WIN32
  ok &= expect(browserEntryMatchesPlaybackTarget(
                   files[0], {"c:\\media\\a.FLAC", -1}),
               "Windows playback matching must use ordinal path identity");
  ok &= expect(browserDirectoryLocation("C:/Media/") ==
                   browserDirectoryLocation("c:\\media"),
               "Windows routes must ignore casing and trailing separators");
#endif

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
  browser.location = browserDirectoryLocation("C:/Media");
  for (int i = 0; i < 30; ++i) {
    browser.entries.push_back(
        fileEntry("Folder " + std::to_string(i),
                  browser.location.path / ("Folder " + std::to_string(i)),
                  true));
  }
  browser.selected = 17;
  browser.scrollRow = 13;
  const BrowserState::Location saved = captureBrowserLocation(browser);

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

#ifdef _WIN32
  BrowserState caseChangedBrowser;
  caseChangedBrowser.location = browserDirectoryLocation("C:/Media");
  caseChangedBrowser.entries = {
      fileEntry("Album", "C:/Media/Album", true)};
  const BrowserState::EntryIdentity caseStableIdentity =
      browserEntryIdentity(caseChangedBrowser.entries.front());
  caseChangedBrowser.entries = {
      fileEntry("ALBUM", "c:\\media\\album", true)};
  ok &= expect(selectBrowserEntry(caseChangedBrowser, caseStableIdentity),
               "selection restore must survive Windows path casing changes");
#endif

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
  historyBrowser.location = browserDirectoryLocation("C:/Media");
  historyBrowser.entries = files;
  historyBrowser.selected = 1;
  historyBrowser.scrollRow = 4;
  const BrowserState::Location parentLocation =
      captureBrowserLocation(historyBrowser);

  bool rejectDirectory = false;
  bool rejectTracks = false;
  BrowserNavigator::Callbacks historyCallbacks;
  historyCallbacks.activate = [&](const BrowserLocation& location) {
    return (location.kind != BrowserLocationKind::Directory ||
            !rejectDirectory) &&
           (location.kind != BrowserLocationKind::TrackBrowser ||
            !rejectTracks);
  };
  historyCallbacks.refresh = [&](const std::string&) {
    historyBrowser.entries =
        historyBrowser.location.kind == BrowserLocationKind::TrackBrowser
            ? tracks
            : files;
  };
  BrowserNavigator historyNavigator(historyBrowser,
                                    std::move(historyCallbacks));
  ok &= expect(historyNavigator.navigate(browserTrackLocation(songA)),
               "a real location change must create one history entry");
  ok &= expect(historyBrowser.backHistory.size() == 1 &&
                   historyBrowser.forwardHistory.empty(),
               "new navigation must own the Back stack and clear Forward");

  historyBrowser.selected = 1;
  historyBrowser.scrollRow = 6;
  const BrowserState::Location trackBrowserLocation =
      captureBrowserLocation(historyBrowser);
  ok &= expect(historyNavigator.back() &&
                   historyBrowser.location == parentLocation.route &&
                   historyBrowser.selected == 1 &&
                   historyBrowser.scrollRow == 4 &&
                   historyBrowser.backHistory.empty() &&
                   historyBrowser.forwardHistory.size() == 1,
               "Back must atomically restore the exact source location");

  ok &= expect(historyNavigator.forward() &&
                   historyBrowser.location == trackBrowserLocation.route &&
                   historyBrowser.selected == 1 &&
                   historyBrowser.scrollRow == 6 &&
                   historyBrowser.backHistory.size() == 1 &&
                   historyBrowser.forwardHistory.empty(),
               "Forward must restore the destination as it was left");
  const BrowserState::Location restoredTrackLocation =
      captureBrowserLocation(historyBrowser);
  ok &= expect(!recordBrowserNavigation(historyBrowser,
                                        restoredTrackLocation,
                                        restoredTrackLocation),
               "playback inside one location must not pollute browser history");

  rejectDirectory = true;
  ok &= expect(!historyNavigator.back() &&
                   historyBrowser.location == trackBrowserLocation.route &&
                   historyBrowser.backHistory.size() == 1 &&
                   historyBrowser.forwardHistory.empty(),
               "a failed Back restore must leave both history stacks intact");
  rejectDirectory = false;
  ok &= expect(historyNavigator.back(),
               "Back must remain usable after a rejected restore");
  rejectTracks = true;
  ok &= expect(!historyNavigator.forward() &&
                   historyBrowser.location == parentLocation.route &&
                   historyBrowser.backHistory.empty() &&
                   historyBrowser.forwardHistory.size() == 1,
               "a failed Forward restore must leave both history stacks intact");
  rejectTracks = false;
  ok &= expect(historyNavigator.forward(),
               "Forward must remain usable after a rejected restore");

  BrowserState locationKindBrowser;
  BrowserState::Location physicalLocation = trackBrowserLocation;
  physicalLocation.route = browserDirectoryLocation(songA);
  ok &= expect(recordBrowserNavigation(locationKindBrowser, physicalLocation,
                                       trackBrowserLocation),
               "physical and virtual locations with one path must differ");

  BrowserState routedBrowser;
  routedBrowser.location = browserDirectoryLocation("C:/Media");
  routedBrowser.entries = files;
  routedBrowser.selected = 1;
  routedBrowser.scrollRow = 4;
  int activated = 0;
  int refreshed = 0;
  bool rejectRoutedDirectory = false;
  BrowserNavigator::Callbacks navigatorCallbacks;
  navigatorCallbacks.activate = [&](const BrowserLocation& location) {
    ++activated;
    return location.kind != BrowserLocationKind::Directory ||
           !rejectRoutedDirectory;
  };
  navigatorCallbacks.refresh = [&](const std::string&) { ++refreshed; };
  BrowserNavigator navigator(routedBrowser, std::move(navigatorCallbacks));
  const BrowserLocation otherDirectory = browserDirectoryLocation("C:/Other");
  ok &= expect(navigator.navigate(otherDirectory) && navigator.back(),
               "the main navigator must create a Forward destination");
  routedBrowser.selected = 1;
  routedBrowser.scrollRow = 4;
  const BrowserState::Location contextOrigin =
      captureBrowserLocation(routedBrowser);
  const size_t mainBackSize = routedBrowser.backHistory.size();
  const size_t mainForwardSize = routedBrowser.forwardHistory.size();

  const BrowserLocation optionsRoot =
      browserOptionsLocation(songA, 3, BrowserOptionsPage::Root);
  const int activatedBeforeContext = activated;
  const int refreshedBeforeContext = refreshed;
  ok &= expect(navigator.navigate(optionsRoot),
               "a contextual route must activate successfully");
  ok &= expect(routedBrowser.location == optionsRoot &&
                   navigator.contextActive() &&
                   routedBrowser.backHistory.size() == mainBackSize &&
                   routedBrowser.forwardHistory.size() == mainForwardSize &&
                   activated == activatedBeforeContext + 1 &&
                   refreshed == refreshedBeforeContext + 1,
               "opening a context must preserve the main history");

  const BrowserLocation optionsLocation = browserOptionsLocation(
      songA, 3, BrowserOptionsPage::VgmDevices);
  ok &= expect(navigator.navigate(optionsLocation) &&
                   routedBrowser.navigationContext &&
                   routedBrowser.navigationContext->backHistory.size() == 1 &&
                   routedBrowser.backHistory.size() == mainBackSize,
               "context navigation must use its own Back stack");
  const BrowserLocation deviceLocation = browserOptionsLocation(
      songA, 3, BrowserOptionsPage::VgmDevice, 0x2612);
  ok &= expect(optionsLocation != deviceLocation,
               "typed options pages must be distinct browser locations");
  ok &= expect(navigator.navigate(deviceLocation) && navigator.back() &&
                   routedBrowser.location == optionsLocation &&
                   routedBrowser.navigationContext &&
                   routedBrowser.navigationContext->forwardHistory.size() ==
                       1 &&
                   navigator.forward() &&
                   routedBrowser.location == deviceLocation,
               "Back and Forward must stay inside the active context");

  const std::optional<BrowserLocation> deviceParent =
      browserOptionsParentLocation(deviceLocation);
  ok &= expect(deviceParent && *deviceParent == optionsLocation &&
                   navigator.navigate(*deviceParent) &&
                   routedBrowser.location == optionsLocation,
               "Options Up must follow the typed parent route");
  const std::optional<BrowserLocation> optionsParent =
      browserOptionsParentLocation(optionsLocation);
  ok &= expect(optionsParent && *optionsParent == optionsRoot &&
                   navigator.navigate(*optionsParent) &&
                   !browserOptionsParentLocation(optionsRoot),
               "the Options root must be the top of the context hierarchy");

  rejectRoutedDirectory = true;
  ok &= expect(!navigator.closeContext() && navigator.contextActive() &&
                   routedBrowser.location == optionsRoot &&
                   routedBrowser.backHistory.size() == mainBackSize &&
                   routedBrowser.forwardHistory.size() == mainForwardSize,
               "a rejected context close must preserve both contexts");
  rejectRoutedDirectory = false;
  ok &= expect(navigator.closeContext() && !navigator.contextActive() &&
                   routedBrowser.location == contextOrigin.route &&
                   routedBrowser.selected == 1 &&
                   routedBrowser.scrollRow == 4 &&
                   routedBrowser.backHistory.size() == mainBackSize &&
                   routedBrowser.forwardHistory.size() == mainForwardSize,
               "closing a context must atomically restore its origin");

  ok &= expect(navigator.navigate(optionsRoot) && navigator.back() &&
                   !navigator.contextActive() &&
                   routedBrowser.location == contextOrigin.route,
               "Back at a context root must close to the origin");
  ok &= expect(navigator.navigate(optionsRoot) &&
                   navigator.navigate(otherDirectory) &&
                   !navigator.contextActive() &&
                   routedBrowser.location == otherDirectory &&
                   routedBrowser.backHistory.size() == mainBackSize + 1 &&
                   routedBrowser.forwardHistory.empty(),
               "leaving a context must record one main navigation from its origin");

  BrowserState rejectedBrowser;
  rejectedBrowser.location = browserDirectoryLocation("C:/Media");
  BrowserNavigator::Callbacks rejectedCallbacks;
  rejectedCallbacks.activate = [](const BrowserLocation&) { return false; };
  BrowserNavigator rejectedNavigator(rejectedBrowser,
                                     std::move(rejectedCallbacks));
  ok &= expect(!rejectedNavigator.navigate(browserTrackLocation(songA)) &&
                   rejectedBrowser.location ==
                       browserDirectoryLocation("C:/Media") &&
                   rejectedBrowser.backHistory.empty(),
               "a rejected content route must not mutate browser state");

#ifdef _WIN32
  const std::optional<std::filesystem::path> driveParent =
      browserParentDirectory(std::filesystem::path("C:\\"));
  ok &= expect(driveParent.has_value() && driveParent->empty(),
               "a drive root must navigate up to the virtual drive list");
#endif

  return ok ? 0 : 1;
}
