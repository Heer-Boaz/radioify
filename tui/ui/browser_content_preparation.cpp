#include "browser_content_preparation.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "browser_directory_listing.h"
#include "browser_content_view.h"
#include "optionsbrowser.h"
#include "track_browser_state.h"
#include "tracklist.h"

namespace browser_content_preparation {
namespace {

struct Populated {};

using PopulationResult =
    std::variant<Populated, BrowserPreparationError, Cancelled>;

bool cancellationWasRequested(
    const CancellationRequested& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

BrowserPreparationError preparationError(
    BrowserPreparationErrorKind kind, const BrowserLocation& location,
    std::string message) {
  return {kind, location, std::move(message)};
}

PopulationResult populate(BrowserState& state,
                          const OptionsBrowserRuntimeSnapshot& optionsRuntime,
                          const CancellationRequested& cancellationRequested) {
  if (cancellationWasRequested(cancellationRequested)) {
    return Cancelled{};
  }

  const bool optionsActive =
      state.location.kind() == BrowserLocationKind::OptionsBrowser;
  if (optionsActive) {
    if (!optionsBrowserSupportsLocation(state.location)) {
      return preparationError(
          BrowserPreparationErrorKind::Unsupported, state.location,
          "Options are not available for this browser location.");
    }
    if (!prepareOptionsBrowserContent(state, optionsRuntime,
                                      cancellationRequested)) {
      if (cancellationWasRequested(cancellationRequested)) {
        return Cancelled{};
      }
      return preparationError(BrowserPreparationErrorKind::Unavailable,
                              state.location,
                              "Unable to prepare media options.");
    }
  } else if (isTrackBrowserActive(state)) {
    std::string trackError;
    std::shared_ptr<const TrackBrowserContent> content =
        prepareTrackBrowserContent(state.location.path(), &trackError);
    if (cancellationWasRequested(cancellationRequested)) {
      return Cancelled{};
    }
    if (!content) {
      return preparationError(
          BrowserPreparationErrorKind::Unavailable, state.location,
          trackError.empty() ? "Unable to prepare the track browser."
                             : std::move(trackError));
    }

    state.content = content;
    state.entries.clear();
    if (content->file.has_parent_path()) {
      state.entries.emplace_back("..", content->file.parent_path(),
                                 browser_entry::NavigateUp{});
    }
    const int digits = trackLabelDigits(content->tracks.size());
    for (const TrackEntry& track : content->tracks) {
      BrowserEntry entry{formatTrackLabel(track, digits), content->file,
                         browser_entry::PlayTrack{track.index}};
      entry.pathIdentity = content->fileIdentity;
      state.entries.push_back(std::move(entry));
    }
  } else {
    browser_directory_listing::Result listing =
        browser_directory_listing::list(state.location.path(),
                                        cancellationRequested);
    if (auto* error =
            std::get_if<browser_directory_listing::Error>(&listing)) {
      return preparationError(BrowserPreparationErrorKind::Unavailable,
                              state.location, std::move(error->message));
    }
    if (std::holds_alternative<browser_directory_listing::Cancelled>(listing)) {
      return Cancelled{};
    }
    state.content = std::monostate{};
    state.entries = std::move(std::get<std::vector<BrowserEntry>>(listing));
  }

  return Populated{};
}

Result prepareImpl(const BrowserContentRequest& request,
                   const CancellationRequested& cancellationRequested) {
  BrowserState candidate;
  candidate.location = request.location;
  candidate.content = request.previousContent;
  candidate.selected = request.selected;
  candidate.sortMode = request.sortMode;
  candidate.sortDescending = request.sortDescending;
  candidate.filter = request.filter;

  PopulationResult population =
      populate(candidate, request.optionsRuntime, cancellationRequested);
  if (auto* error = std::get_if<BrowserPreparationError>(&population)) {
    return std::move(*error);
  }
  if (std::holds_alternative<Cancelled>(population)) {
    return Cancelled{};
  }

  browser_content_view::Request viewRequest;
  viewRequest.initialName = request.initialName;
  if (request.location.kind() == BrowserLocationKind::OptionsBrowser) {
    viewRequest.entryOrder = browser_content_view::EntryOrder::PreserveSource;
  }
  if (browser_content_view::apply(candidate, viewRequest,
                                  cancellationRequested) ==
      browser_content_view::Outcome::Cancelled) {
    return Cancelled{};
  }

  PreparedBrowserContent prepared;
  prepared.entries = std::move(candidate.entries);
  prepared.content = std::move(candidate.content);
  prepared.selected = candidate.selected;
  prepared.scrollRow = candidate.scrollRow;
  prepared.viewportRestoreMode = candidate.viewportRestoreMode;
  prepared.viewportRestoreScrollRow = candidate.viewportRestoreScrollRow;
  return prepared;
}

}  // namespace

Result prepare(const BrowserContentRequest& request,
               CancellationRequested cancellationRequested) {
  try {
    return prepareImpl(request, cancellationRequested);
  } catch (const std::exception& error) {
    return preparationError(BrowserPreparationErrorKind::Internal,
                            request.location, error.what());
  } catch (...) {
    return preparationError(BrowserPreparationErrorKind::Internal,
                            request.location,
                            "Unexpected browser preparation failure.");
  }
}

}  // namespace browser_content_preparation
