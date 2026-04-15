#include "flacaudio.h"

#include <new>

#include "miniaudio.h"
#include "miniaudio_file_path.h"

struct FlacAudioDecoder::Impl {
  ma_decoder decoder{};
};

namespace {
void setError(std::string* error, const char* message) {
  if (error) *error = message;
}
}  // namespace

FlacAudioDecoder::FlacAudioDecoder() = default;

FlacAudioDecoder::~FlacAudioDecoder() {
  uninit();
}

bool FlacAudioDecoder::init(const std::filesystem::path& path,
                            uint32_t channels,
                            uint32_t sampleRate,
                            std::string* error) {
  uninit();

  Impl* impl = new (std::nothrow) Impl();
  if (!impl) {
    setError(error, "Out of memory while creating FLAC decoder.");
    return false;
  }

  ma_decoder_config decConfig =
      ma_decoder_config_init(ma_format_f32, channels, sampleRate);
  ma_result result = maDecoderInitFilePath(path, &decConfig, &impl->decoder);
  if (result != MA_SUCCESS) {
    delete impl;
    setError(error, "Failed to open FLAC file.");
    return false;
  }

  impl_ = impl;
  return true;
}

void FlacAudioDecoder::uninit() {
  if (!impl_) return;
  ma_decoder_uninit(&impl_->decoder);
  delete impl_;
  impl_ = nullptr;
}

bool FlacAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                  uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!impl_ || !out) return false;
  if (frameCount == 0) return true;

  ma_uint64 read = 0;
  ma_result result =
      ma_decoder_read_pcm_frames(&impl_->decoder, out, frameCount, &read);
  if (framesRead) *framesRead = static_cast<uint64_t>(read);
  return result == MA_SUCCESS || result == MA_AT_END;
}

bool FlacAudioDecoder::seekToFrame(uint64_t frame) {
  if (!impl_) return false;
  return ma_decoder_seek_to_pcm_frame(&impl_->decoder,
                                      static_cast<ma_uint64>(frame)) ==
         MA_SUCCESS;
}

bool FlacAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames || !impl_) return false;
  ma_uint64 total = 0;
  if (ma_decoder_get_length_in_pcm_frames(&impl_->decoder, &total) !=
      MA_SUCCESS) {
    *outFrames = 0;
    return false;
  }
  *outFrames = static_cast<uint64_t>(total);
  return true;
}
