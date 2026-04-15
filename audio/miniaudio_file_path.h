#ifndef RADIOIFY_AUDIO_MINIAUDIO_FILE_PATH_H
#define RADIOIFY_AUDIO_MINIAUDIO_FILE_PATH_H

#include <filesystem>
#include <string>

#include "miniaudio.h"
#include "runtime_helpers.h"

inline ma_result maDecoderInitFilePath(const std::filesystem::path& path,
                                       const ma_decoder_config* config,
                                       ma_decoder* decoder) {
#ifdef _WIN32
  return ma_decoder_init_file_w(path.c_str(), config, decoder);
#else
  const std::string pathUtf8 = toUtf8String(path);
  return ma_decoder_init_file(pathUtf8.c_str(), config, decoder);
#endif
}

inline ma_result maEncoderInitFilePath(const std::filesystem::path& path,
                                       const ma_encoder_config* config,
                                       ma_encoder* encoder) {
#ifdef _WIN32
  return ma_encoder_init_file_w(path.c_str(), config, encoder);
#else
  const std::string pathUtf8 = toUtf8String(path);
  return ma_encoder_init_file(pathUtf8.c_str(), config, encoder);
#endif
}

#endif  // RADIOIFY_AUDIO_MINIAUDIO_FILE_PATH_H
