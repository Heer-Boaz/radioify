#pragma once

#include <filesystem>
#include <string>

#include "browser_model.h"

std::string buildSelectionMeta(const BrowserState& browser,
                               bool (*isVideo)(const std::filesystem::path&));
