#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

bool initM4aBackend(const std::filesystem::path& file,
                    uint64_t startFrame,
                    int trackIndex,
                    std::string* error);
void uninitM4aBackend();
bool totalM4aBackend(uint64_t* outFrames);
