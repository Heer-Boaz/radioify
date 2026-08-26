#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "browser_model.h"

namespace browser_directory_listing {

using CancellationRequested = std::function<bool()>;

struct Cancelled {};

struct Error {
  std::string message;
};

using Result = std::variant<std::vector<BrowserEntry>, Cancelled, Error>;

Result list(const std::filesystem::path& directory,
            CancellationRequested cancellationRequested = {});

}  // namespace browser_directory_listing
