#ifndef ASCIIART_IMAGE_DECODE_WIC_H
#define ASCIIART_IMAGE_DECODE_WIC_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

bool decodeImageFileToRgba(const std::filesystem::path& path, int& outWidth,
                           int& outHeight, std::vector<uint8_t>& outPixels,
                           std::string* error);

bool decodeImageBytesToRgba(const uint8_t* bytes, size_t size, int& outWidth,
                            int& outHeight, std::vector<uint8_t>& outPixels,
                            std::string* error);

#endif
