#include "tui/ui/browser_content_view.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "browser_content_view_tests: " << message << '\n';
  return false;
}

BrowserEntry mediaEntry(const std::string& name) {
  return BrowserEntry{name, std::filesystem::path("media") / name,
                      browser_entry::OpenFile{}};
}

}  // namespace

int main() {
  bool ok = true;

  BrowserState filtered;
  filtered.location = browserDirectoryLocation("media");
  filtered.entries.emplace_back("..", std::filesystem::path{},
                                browser_entry::NavigateUp{});
  filtered.entries.push_back(mediaEntry("Beta.mp4"));
  filtered.entries.push_back(mediaEntry("Alpha.mp4"));
  filtered.filter = "ALP";
  filtered.selected = 99;
  filtered.scrollRow = 12;
  browser_content_view::Request filteredRequest;
  filteredRequest.initialName = "alpha.MP4";
  const browser_content_view::Outcome filteredOutcome =
      browser_content_view::apply(filtered, filteredRequest);
  ok &= expect(filteredOutcome == browser_content_view::Outcome::Completed,
               "content projection must complete without cancellation");
  ok &= expect(filtered.entries.size() == 2 &&
                   filtered.entries[0].actionAs<browser_entry::NavigateUp>() &&
                   filtered.entries[1].name == "Alpha.mp4",
               "filtering must be case-insensitive and retain parent navigation");
  ok &= expect(
      filtered.selected == 1 && filtered.scrollRow == 0 &&
          filtered.viewportRestoreMode ==
              BrowserState::ViewportRestoreMode::RevealSelection,
      "initial-name matching must select and reveal the requested entry");

  BrowserState sourceOrdered;
  sourceOrdered.location = browserDirectoryLocation("media");
  sourceOrdered.entries.push_back(mediaEntry("Zebra.mp4"));
  sourceOrdered.entries.push_back(mediaEntry("Alpha.mp4"));
  browser_content_view::Request sourceOrderRequest;
  sourceOrderRequest.entryOrder =
      browser_content_view::EntryOrder::PreserveSource;
  browser_content_view::apply(sourceOrdered, sourceOrderRequest);
  ok &= expect(sourceOrdered.entries[0].name == "Zebra.mp4" &&
                   sourceOrdered.entries[1].name == "Alpha.mp4",
               "source-owned option ordering must remain stable");

  BrowserState sorted;
  sorted.location = browserDirectoryLocation("media");
  sorted.entries.push_back(mediaEntry("Zebra.mp4"));
  sorted.entries.push_back(mediaEntry("Alpha.mp4"));
  browser_content_view::Request sortedRequest;
  browser_content_view::apply(sorted, sortedRequest);
  ok &= expect(sorted.entries[0].name == "Alpha.mp4" &&
                   sorted.entries[1].name == "Zebra.mp4",
               "ordinary browser content must use the configured sort policy");

  BrowserState cancelled;
  cancelled.location = browserDirectoryLocation("media");
  cancelled.entries.push_back(mediaEntry("Beta.mp4"));
  cancelled.entries.push_back(mediaEntry("Alpha.mp4"));
  browser_content_view::Request cancelledRequest;
  const browser_content_view::Outcome cancelledOutcome =
      browser_content_view::apply(cancelled, cancelledRequest,
                                  [] { return true; });
  ok &= expect(cancelledOutcome == browser_content_view::Outcome::Cancelled,
               "cancelled projections must not be published as completed");

  BrowserState empty;
  empty.selected = 8;
  empty.scrollRow = 5;
  browser_content_view::Request emptyRequest;
  browser_content_view::apply(empty, emptyRequest);
  ok &= expect(empty.selected == 0 && empty.scrollRow == 0,
               "empty content must reset stale selection and scrolling");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
