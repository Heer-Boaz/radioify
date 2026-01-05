#pragma once

#include <filesystem>
#include <string>

struct BrowserState;

std::string buildSelectionMeta(const BrowserState& browser,
                               bool (*isVideo)(const std::filesystem::path&));
