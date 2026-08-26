#pragma once

#include <functional>
#include <variant>

#include "browser_navigation.h"

namespace browser_content_preparation {

using CancellationRequested = std::function<bool()>;

struct Cancelled {};

using Result =
    std::variant<PreparedBrowserContent, BrowserPreparationError, Cancelled>;

Result prepare(const BrowserContentRequest& request,
               CancellationRequested cancellationRequested = {});

}  // namespace browser_content_preparation
