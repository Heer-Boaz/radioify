#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "browser_navigation.h"
#include "core/latest_request_worker.h"
#include "kssoptions.h"
#include "optionsbrowser.h"
#include "playback_target_match.h"
#include "track_browser_state.h"
#include "ui_inputlogic.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "browser_navigation_tests: " << message << '\n';
    return false;
  }
  return true;
}

BrowserEntry fileEntry(const std::string& name,
                       const std::filesystem::path& path) {
  return {name, path, browser_entry::OpenFile{}};
}

BrowserEntry directoryEntry(const std::string& name,
                            const std::filesystem::path& path) {
  return {name, path, browser_entry::OpenDirectory{}};
}

BrowserEntry trackEntry(const std::string& name,
                        const std::filesystem::path& path, int trackIndex) {
  return {name, path, browser_entry::PlayTrack{trackIndex}};
}

BrowserPreparationError unavailableBrowserContent(
    const BrowserLocation& location,
    std::string message = "Browser content is unavailable.") {
  return {BrowserPreparationErrorKind::Unavailable, location,
          std::move(message)};
}

GridLayout verticalLayout(int totalRows, int visibleRows) {
  GridLayout layout;
  layout.totalRows = totalRows;
  layout.rowsVisible = visibleRows;
  layout.cols = 1;
  return layout;
}

bool hasTrackContent(
    const BrowserState& browser,
    const std::shared_ptr<const TrackBrowserContent>& expected) {
  const auto* content =
      std::get_if<std::shared_ptr<const TrackBrowserContent>>(
          &browser.content);
  return content && content->get() == expected.get();
}

class FakeBrowserPreparationService final
    : public BrowserPreparationService {
 public:
  using Prepare = std::function<BrowserContentPreparation(
      BrowserPreparationId, const BrowserContentRequest&)>;
  using Cancel = std::function<void(BrowserPreparationId)>;

  BrowserContentPreparation prepare(
      BrowserPreparationId preparationId,
      const BrowserContentRequest& request) override {
    return prepareRequest(preparationId, request);
  }

  void cancelThrough(BrowserPreparationId generation) override {
    if (cancelGeneration) cancelGeneration(generation);
  }

  Prepare prepareRequest;
  Cancel cancelGeneration;
};

struct NavigationEventCounts {
  int changed = 0;
  int failed = 0;
};

NavigationEventCounts drainNavigationEvents(BrowserNavigator& navigator) {
  NavigationEventCounts counts;
  for (const BrowserNavigator::Event& event : navigator.drainEvents()) {
    if (std::holds_alternative<BrowserNavigator::Changed>(event)) {
      ++counts.changed;
    } else {
      ++counts.failed;
    }
  }
  return counts;
}

}  // namespace

int main() {
  bool ok = true;

  const std::filesystem::path songA = "C:/Media/A.flac";
  const std::filesystem::path songB = "C:/Media/B.flac";
  const std::filesystem::path kssFile = "C:/Media/Game.kss";
  const BrowserEntry status{"Scan failed", {}, browser_entry::Status{}};
  const BrowserEntry information{"Title: Example", {},
                                 browser_entry::Information{}};
  ok &= expect(!status.isSelectable() && !status.isActivatable(),
               "status rows must not masquerade as interactive files");
  ok &= expect(information.isSelectable() && !information.isActivatable(),
               "informational rows must be focusable without being actions");

  std::vector<BrowserEntry> files{fileEntry("A.flac", songA),
                                  fileEntry("B.flac", songB)};

  BrowserState sortedBrowser;
  sortedBrowser.location = browserDirectoryLocation("C:/Media");
  BrowserEntry older = fileEntry("Equal.flac", "C:/Media/older.flac");
  BrowserEntry newer = fileEntry("Equal.flac", "C:/Media/newer.flac");
  const auto now = std::filesystem::file_time_type::clock::now();
  older.sortMetadata.modifiedAt = now - std::chrono::hours(1);
  newer.sortMetadata.modifiedAt = now;
  older.sortMetadata.size = 100;
  newer.sortMetadata.size = 200;
  sortedBrowser.entries = {newer, older};
  sortedBrowser.sortMode = BrowserState::SortMode::Date;
  sortBrowserEntries(sortedBrowser);
  ok &= expect(sortedBrowser.entries.front().path == older.path,
               "date sorting must use the enumerated metadata snapshot");
  sortedBrowser.sortDescending = true;
  sortBrowserEntries(sortedBrowser);
  ok &= expect(sortedBrowser.entries.front().path == newer.path,
               "descending sorting must reverse a strict ordering");
  sortedBrowser.sortMode = BrowserState::SortMode::Size;
  sortBrowserEntries(sortedBrowser);
  ok &= expect(sortedBrowser.entries.front().path == newer.path,
               "equal names must remain strictly ordered by their size key");
  browser_input::EntryClickTracker entryClickTracker;
  entryClickTracker.recordPress(files[0]);
  const auto sameEntryAnchor =
      entryClickTracker.consumeDoubleClickAnchor();
  ok &= expect(sameEntryAnchor &&
                   browserEntryMatchesIdentity(files[0], *sameEntryAnchor),
               "a double-click must retain the identity of its first entry");
  ok &= expect(!entryClickTracker.consumeDoubleClickAnchor(),
               "a completed double-click must consume its entry anchor");

  BrowserPointerState pointerState;
  pointerState.pressAction(ActionStripItem::Radio);
  ok &= expect(pointerState.hasPressedAction(),
               "an action button press must establish pointer capture");
  ok &= expect(!pointerState.releaseAction(ActionStripItem::Options) &&
                   !pointerState.hasPressedAction(),
               "releasing over another button must cancel the action");
  pointerState.pressAction(ActionStripItem::Radio);
  const auto releasedAction =
      pointerState.releaseAction(ActionStripItem::Radio);
  ok &= expect(releasedAction && *releasedAction == ActionStripItem::Radio &&
                   !pointerState.hasPressedAction(),
               "a button action must fire once on matching release");

  BrowserState pointerBrowser;
  pointerBrowser.entries = files;
  pointerBrowser.selected = 0;
  ok &= expect(setBrowserHoveredEntry(pointerBrowser, 1),
               "moving hover to another entry must change pointer state");
  ok &= expect(pointerBrowser.selected == 0 && pointerBrowser.hovered == 1,
               "pointer hover must not mutate the browser selection");
  entryClickTracker.recordPress(files[0]);
  const auto differentEntryAnchor =
      entryClickTracker.consumeDoubleClickAnchor();
  ok &= expect(differentEntryAnchor &&
                   !browserEntryMatchesIdentity(files[1],
                                                *differentEntryAnchor),
               "a second press on another entry must not activate it");
  const PlaybackTarget playingA = playbackFileTarget(songA);
  ok &= expect(findBrowserPlaybackTargetEntry(files, playingA) == 0,
               "the playing item must be found independently of selection");
  ok &= expect(
      browserEntryMatchesPlaybackTarget(
          files[0], playbackFileTarget("C:/Media/./A.flac")),
      "lexically equivalent playback paths must match");
#ifdef _WIN32
  ok &= expect(browserEntryMatchesPlaybackTarget(
                   files[0], playbackFileTarget("c:\\media\\a.FLAC")),
               "Windows playback matching must use ordinal path identity");
  ok &= expect(browserDirectoryLocation("C:/Media/") ==
                   browserDirectoryLocation("c:\\media"),
               "Windows routes must ignore casing and trailing separators");
#endif

  const PlaybackTarget playingContainerTrack =
      *playbackTrackTarget(songA, 3);
  ok &=
      expect(browserEntryMatchesPlaybackTarget(files[0], playingContainerTrack),
             "a container file must remain active for an internal track");
  std::vector<BrowserEntry> tracks{trackEntry("Track 1", songA, 0),
                                   trackEntry("Track 4", songA, 3)};
  ok &=
      expect(findBrowserPlaybackTargetEntry(tracks, playingContainerTrack) == 1,
             "a track browser must mark only the exact playing track");
  ok &= expect(!browserEntryMatchesPlaybackTarget(
                   directoryEntry("Media", "C:/Media"), playingA),
               "directories must never receive a playback marker");

  ok &= expect(!optionsBrowserSubjectForEntry(files.front()),
               "options must not fall back from an unsupported selection");
  const BrowserEntry kssContainer = fileEntry("Game.kss", kssFile);
  const auto kssContainerSubject =
      optionsBrowserSubjectForEntry(kssContainer);
  ok &= expect(kssContainerSubject &&
                   kssContainerSubject->file == kssFile &&
                   kssContainerSubject->trackIndex == 0,
               "a KSS container subject must explicitly select its first "
               "track");
  const BrowserEntry kssTrack = trackEntry("Track 5", kssFile, 4);
  const auto kssTrackSubject = optionsBrowserSubjectForEntry(kssTrack);
  ok &= expect(kssTrackSubject && kssTrackSubject->trackIndex == 4 &&
                   browserOptionsTrackIndex(
                       optionsBrowserOpenLocation(*kssTrackSubject)) == 4,
               "a selected internal track must remain the options subject");

  BrowserState optionIdentityBrowser;
  optionIdentityBrowser.location =
      browserOptionsLocation(songA, uint32_t{0}, BrowserOptionsRoot{});
  optionIdentityBrowser.entries.emplace_back(
      "50Hz: auto", std::filesystem::path{},
      browser_entry::AdjustKssOption{KssOptionId::Force50Hz});
  const BrowserState::Location optionIdentity =
      captureBrowserLocation(optionIdentityBrowser);
  optionIdentityBrowser.entries.front() = BrowserEntry{
      "50Hz: forced", {},
      browser_entry::AdjustKssOption{KssOptionId::Force50Hz}};
  ok &= expect(restoreBrowserLocation(optionIdentityBrowser, optionIdentity),
               "entry identity must use the typed option action, not its label");

  BrowserState browser;
  browser.viewMode = BrowserState::ViewMode::Thumbnails;
  browser.location = browserDirectoryLocation("C:/Media");
  for (int i = 0; i < 30; ++i) {
    browser.entries.push_back(
        directoryEntry(
            "Folder " + std::to_string(i),
            browser.location.path() / ("Folder " + std::to_string(i))));
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

  const BrowserEntry moved = browser.entries[17];
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
      directoryEntry("Album", "C:/Media/Album")};
  const BrowserState::EntryIdentity caseStableIdentity =
      browserEntryIdentity(caseChangedBrowser.entries.front());
  caseChangedBrowser.entries = {
      directoryEntry("ALBUM", "c:\\media\\album")};
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
  FakeBrowserPreparationService historyPreparation;
  historyPreparation.prepareRequest =
      [&](BrowserPreparationId, const BrowserContentRequest& request) {
    if ((request.location.kind() == BrowserLocationKind::Directory &&
         rejectDirectory) ||
        (request.location.kind() == BrowserLocationKind::TrackBrowser &&
         rejectTracks)) {
      return BrowserContentPreparation::failed(
          unavailableBrowserContent(request.location));
    }
    PreparedBrowserContent prepared;
    prepared.entries =
        request.location.kind() == BrowserLocationKind::TrackBrowser ? tracks
                                                                   : files;
    prepared.selected = request.selected;
    return BrowserContentPreparation::complete(std::move(prepared));
  };
  BrowserNavigator historyNavigator(historyBrowser, historyPreparation);
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
                   !historyBrowser.contentError.empty() &&
                   historyBrowser.location == trackBrowserLocation.route &&
                   historyBrowser.entries.size() == tracks.size() &&
                   historyBrowser.entries[1]
                           .actionAs<browser_entry::PlayTrack>() &&
                   historyBrowser.entries[1]
                           .actionAs<browser_entry::PlayTrack>()
                           ->trackIndex == 3 &&
                   historyBrowser.backHistory.size() == 1 &&
                   historyBrowser.forwardHistory.empty(),
               "a failed Back restore must leave content and history intact");
  rejectDirectory = false;
  ok &= expect(historyNavigator.back() && historyBrowser.contentError.empty(),
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
  int preparedCount = 0;
  bool rejectRoutedDirectory = false;
  FakeBrowserPreparationService routedPreparation;
  routedPreparation.prepareRequest =
      [&](BrowserPreparationId, const BrowserContentRequest& request) {
    ++preparedCount;
    if (request.location.kind() == BrowserLocationKind::Directory &&
        rejectRoutedDirectory) {
      return BrowserContentPreparation::failed(
          unavailableBrowserContent(request.location));
    }
    PreparedBrowserContent prepared;
    prepared.entries = routedBrowser.entries;
    prepared.selected = request.selected;
    return BrowserContentPreparation::complete(std::move(prepared));
  };
  BrowserNavigator navigator(routedBrowser, routedPreparation);
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
      browserOptionsLocation(songA, uint32_t{3}, BrowserOptionsRoot{});
  const int preparedBeforeContext = preparedCount;
  ok &= expect(navigator.navigate(optionsRoot),
               "a contextual route must activate successfully");
  ok &= expect(routedBrowser.location == optionsRoot &&
                   navigator.contextActive() &&
                   routedBrowser.backHistory.size() == mainBackSize &&
                   routedBrowser.forwardHistory.size() == mainForwardSize &&
                   preparedCount == preparedBeforeContext + 1,
               "opening a context must preserve the main history");

  const BrowserLocation optionsLocation = browserOptionsLocation(
      songA, uint32_t{3}, BrowserOptionsVgmDevices{});
  ok &= expect(navigator.navigate(optionsLocation) &&
                   routedBrowser.navigationContext &&
                   routedBrowser.navigationContext->backHistory.size() == 1 &&
                   routedBrowser.backHistory.size() == mainBackSize,
               "context navigation must use its own Back stack");
  const BrowserLocation deviceLocation = browserOptionsLocation(
      songA, uint32_t{3}, BrowserOptionsVgmDevice{0x2612});
  ok &= expect(optionsLocation != deviceLocation,
               "typed options pages must be distinct browser locations");
  const BrowserLocation instrumentTrack3 = browserOptionsLocation(
      songA, uint32_t{3}, BrowserOptionsInstruments{3});
  const BrowserLocation instrumentTrack4 = browserOptionsLocation(
      songA, uint32_t{3}, BrowserOptionsInstruments{4});
  ok &= expect(instrumentTrack3 != instrumentTrack4 &&
                   browserOptionsInstrumentTrackIndex(instrumentTrack3) == 3,
               "an instrument route must own its required track context");
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

  BrowserState asynchronousBrowser;
  asynchronousBrowser.location = browserDirectoryLocation("C:/Media");
  asynchronousBrowser.entries = files;
  asynchronousBrowser.selected = 1;
  asynchronousBrowser.scrollRow = 3;
  std::vector<BrowserPreparationId> asynchronousPreparationIds;
  std::vector<BrowserPreparationId> asynchronousCancellationIds;
  FakeBrowserPreparationService asynchronousPreparation;
  asynchronousPreparation.prepareRequest =
      [&](BrowserPreparationId preparationId,
          const BrowserContentRequest&) {
        asynchronousPreparationIds.push_back(preparationId);
        return BrowserContentPreparation::pending();
      };
  asynchronousPreparation.cancelGeneration =
      [&](BrowserPreparationId cancellationId) {
        asynchronousCancellationIds.push_back(cancellationId);
      };
  BrowserNavigator asynchronousNavigator(asynchronousBrowser,
                                          asynchronousPreparation);

  ok &= expect(
      asynchronousNavigator.navigate(browserTrackLocation(songA)) &&
          asynchronousBrowser.contentLoading &&
          asynchronousBrowser.location == browserDirectoryLocation("C:/Media") &&
          asynchronousBrowser.entries.size() == files.size() &&
          asynchronousBrowser.backHistory.empty(),
      "pending preparation must preserve the committed browser snapshot");
  ok &= expect(asynchronousNavigator.navigate(otherDirectory) &&
                   asynchronousPreparationIds.size() == 2 &&
                   asynchronousPreparationIds[0] !=
                       asynchronousPreparationIds[1] &&
                   asynchronousCancellationIds.size() == 1 &&
                   asynchronousCancellationIds.front() ==
                       asynchronousPreparationIds[1],
               "a newer navigation must receive a distinct preparation generation");

  PreparedBrowserContent stalePrepared;
  stalePrepared.entries = tracks;
  ok &= expect(
      !asynchronousNavigator.completePreparation(
          asynchronousPreparationIds[0], std::move(stalePrepared)) &&
          asynchronousBrowser.contentLoading &&
          asynchronousBrowser.location == browserDirectoryLocation("C:/Media"),
      "a stale completion must not commit after a newer navigation request");

  PreparedBrowserContent newestPrepared;
  newestPrepared.entries = {directoryEntry("Archive", "C:/Other/Archive")};
  const bool newestPreparationCommitted =
      asynchronousNavigator.completePreparation(
          asynchronousPreparationIds[1], std::move(newestPrepared));
  const NavigationEventCounts asynchronousEvents =
      drainNavigationEvents(asynchronousNavigator);
  ok &= expect(
      newestPreparationCommitted &&
          !asynchronousBrowser.contentLoading &&
          asynchronousBrowser.location == otherDirectory &&
          asynchronousBrowser.backHistory.size() == 1 &&
          asynchronousEvents.changed == 3 && asynchronousEvents.failed == 0,
      "only the newest preparation may atomically commit content and history");

  const size_t asynchronousHistorySize =
      asynchronousBrowser.backHistory.size();
  ok &= expect(asynchronousNavigator.navigate(browserTrackLocation(songB)),
               "an asynchronous failure test must be accepted first");
  const BrowserPreparationId failedPreparationId =
      asynchronousPreparationIds.back();
  ok &= expect(
      !asynchronousNavigator.completePreparation(failedPreparationId,
          unavailableBrowserContent(browserTrackLocation(songB),
                                    "Track scan failed.")) &&
          !asynchronousBrowser.contentLoading &&
          asynchronousBrowser.contentError == "Track scan failed." &&
          asynchronousBrowser.location == otherDirectory &&
          asynchronousBrowser.backHistory.size() == asynchronousHistorySize,
      "failed asynchronous preparation must preserve content and history");

  ok &= expect(asynchronousNavigator.navigate(browserTrackLocation(songA)),
               "a cancellable navigation must first enter preparation");
  const BrowserPreparationId cancelledPreparationId =
      asynchronousPreparationIds.back();
  ok &= expect(
      asynchronousNavigator.back() &&
          !asynchronousBrowser.contentLoading &&
          asynchronousBrowser.location == otherDirectory &&
          asynchronousBrowser.backHistory.size() == asynchronousHistorySize &&
          asynchronousCancellationIds.size() == 2 &&
          asynchronousCancellationIds.back() != cancelledPreparationId,
      "Back during preparation must cancel the uncommitted navigation");
  PreparedBrowserContent cancelledPrepared;
  cancelledPrepared.entries = tracks;
  ok &= expect(
      !asynchronousNavigator.completePreparation(
          cancelledPreparationId, std::move(cancelledPrepared)) &&
          asynchronousBrowser.location == otherDirectory,
      "a completion arriving after cancellation must remain stale");

  BrowserState revealBrowser;
  revealBrowser.location = browserDirectoryLocation("C:/Media");
  revealBrowser.entries = {fileEntry("B.flac", songB)};
  revealBrowser.filter = "B";
  revealBrowser.pathSearch = "stale path query";
  revealBrowser.searchFocus = BrowserSearchFocus::PathSearch;
  BrowserPreparationId revealPreparationId = 0;
  std::optional<BrowserContentRequest> revealRequest;
  FakeBrowserPreparationService revealPreparation;
  revealPreparation.prepareRequest =
      [&](BrowserPreparationId preparationId,
          const BrowserContentRequest& request) {
        revealPreparationId = preparationId;
        revealRequest = request;
        return BrowserContentPreparation::pending();
      };
  BrowserNavigator revealNavigator(revealBrowser, revealPreparation);
  const BrowserState::EntryIdentity revealIdentity =
      browserEntryIdentity(files.front());
  ok &= expect(
          revealNavigator.reveal(revealBrowser.location, "A.flac",
                             revealIdentity) &&
          revealRequest && revealRequest->filter.empty() &&
          revealBrowser.filter == "B" &&
          revealBrowser.searchFocus == BrowserSearchFocus::PathSearch,
      "reveal preparation must reset search in its candidate, not live state");
  PreparedBrowserContent revealPrepared;
  revealPrepared.entries = files;
  ok &= expect(
      revealNavigator.completePreparation(revealPreparationId,
                                          std::move(revealPrepared)) &&
          revealBrowser.filter.empty() &&
          revealBrowser.pathSearch.empty() &&
          revealBrowser.searchFocus == BrowserSearchFocus::None &&
          revealBrowser.selected == 0,
      "reveal must atomically commit unfiltered content and its selection");

  {
    using TestWorker = LatestRequestWorker<int, int>;
    std::mutex workerMutex;
    std::condition_variable workerChanged;
    bool firstStarted = false;
    bool releaseFirst = false;
    TestWorker worker(
        [&](int request, const TestWorker::Cancellation& cancellation)
            -> std::optional<int> {
          if (request == 1) {
            std::unique_lock<std::mutex> lock(workerMutex);
            firstStarted = true;
            workerChanged.notify_one();
            workerChanged.wait(lock, [&]() { return releaseFirst; });
          }
          if (cancellation.requested()) {
            return std::nullopt;
          }
          return request * 10;
        });

    ok &= expect(worker.submit(1, 1),
                 "the latest-request worker must accept work while active");
    {
      std::unique_lock<std::mutex> lock(workerMutex);
      ok &= expect(workerChanged.wait_for(
                       lock, std::chrono::seconds(2),
                       [&]() { return firstStarted; }),
                   "the worker must begin the first request");
    }
    ok &= expect(worker.submit(2, 2),
                 "a newer request must replace pending work");
    {
      std::lock_guard<std::mutex> lock(workerMutex);
      releaseFirst = true;
    }
    workerChanged.notify_one();

    std::optional<TestWorker::Completion> completion;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(completion = worker.poll()) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= expect(completion && completion->generation == 2 &&
                     completion->result && *completion->result == 20,
                 "the worker must publish only the newest generation");
  }

  BrowserState rejectedBrowser;
  rejectedBrowser.location = browserDirectoryLocation("C:/Media");
  rejectedBrowser.entries = files;
  rejectedBrowser.selected = 1;
  rejectedBrowser.scrollRow = 3;
  rejectedBrowser.filter = "B";
  auto committedTrackContent = std::make_shared<TrackBrowserContent>();
  committedTrackContent->file = songB;
  rejectedBrowser.content =
      std::shared_ptr<const TrackBrowserContent>(committedTrackContent);
  int rejectedChangedCount = 0;
  int rejectedFailureCount = 0;
  FakeBrowserPreparationService rejectedPreparation;
  rejectedPreparation.prepareRequest =
      [](BrowserPreparationId, const BrowserContentRequest& request) {
    return BrowserContentPreparation::failed(
        unavailableBrowserContent(request.location));
  };
  BrowserNavigator rejectedNavigator(rejectedBrowser, rejectedPreparation);
  const bool rejectedNavigation =
      rejectedNavigator.navigate(browserTrackLocation(songA));
  NavigationEventCounts rejectedEvents =
      drainNavigationEvents(rejectedNavigator);
  rejectedChangedCount += rejectedEvents.changed;
  rejectedFailureCount += rejectedEvents.failed;
  ok &= expect(!rejectedNavigation &&
                   rejectedBrowser.location ==
                       browserDirectoryLocation("C:/Media") &&
                   rejectedBrowser.entries.size() == files.size() &&
                   rejectedBrowser.entries[1].pathIdentity ==
                       files[1].pathIdentity &&
                   rejectedBrowser.selected == 1 &&
                   rejectedBrowser.scrollRow == 3 &&
                   rejectedBrowser.filter == "B" &&
                   hasTrackContent(rejectedBrowser, committedTrackContent) &&
                   rejectedBrowser.backHistory.empty() &&
                   !rejectedBrowser.contentError.empty() &&
                   rejectedFailureCount == 1 && rejectedChangedCount == 1,
               "a rejected content snapshot must not mutate browser state");
  const bool rejectedReload = rejectedNavigator.reload();
  rejectedEvents = drainNavigationEvents(rejectedNavigator);
  rejectedChangedCount += rejectedEvents.changed;
  rejectedFailureCount += rejectedEvents.failed;
  ok &= expect(!rejectedReload &&
                   rejectedBrowser.entries.size() == files.size() &&
                   rejectedBrowser.selected == 1 &&
                   rejectedBrowser.scrollRow == 3 &&
                   rejectedBrowser.filter == "B" &&
                   hasTrackContent(rejectedBrowser, committedTrackContent) &&
                   !rejectedBrowser.contentError.empty() &&
                   rejectedFailureCount == 2 && rejectedChangedCount == 2,
               "a failed reload must preserve the committed snapshot");

#ifdef _WIN32
  const std::optional<std::filesystem::path> driveParent =
      browserParentDirectory(std::filesystem::path("C:\\"));
  ok &= expect(driveParent.has_value() && driveParent->empty(),
               "a drive root must navigate up to the virtual drive list");
#endif

  return ok ? 0 : 1;
}
