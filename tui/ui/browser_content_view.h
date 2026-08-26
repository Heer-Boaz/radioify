#pragma once

#include <functional>
#include <string>

#include "browser_model.h"

namespace browser_content_view {

using CancellationRequested = std::function<bool()>;

enum class EntryOrder {
  Sorted,
  PreserveSource,
};

struct Request {
  std::string initialName;
  EntryOrder entryOrder = EntryOrder::Sorted;
};

enum class Outcome {
  Completed,
  Cancelled,
};

Outcome apply(BrowserState& state, const Request& request,
              CancellationRequested cancellationRequested = {});

}  // namespace browser_content_view
