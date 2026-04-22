#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "consoleinput.h"

bool dispatchFileDrop(
    const InputEvent& ev,
    const std::function<bool(const std::vector<std::filesystem::path>&)>&
        openFiles);
