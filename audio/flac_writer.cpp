#include "audio/flac_writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "runtime_helpers.h"

namespace audio_file {
namespace {

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string ffmpegError(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

}  // namespace

struct FlacWriter::Impl {
  AVFormatContext* format = nullptr;
  AVCodecContext* encoder = nullptr;
  AVStream* stream = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  std::uint32_t channels = 0;
  std::vector<float> pending;
  std::size_t pendingOffset = 0;
  std::int64_t nextPts = 0;
  bool headerWritten = false;
  bool finished = false;

  ~Impl() {
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    if (format) {
      if (format->pb && !(format->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&format->pb);
      }
      avformat_free_context(format);
    }
  }

  bool receivePackets(std::string* error) {
    for (;;) {
      av_packet_unref(packet);
      const int result = avcodec_receive_packet(encoder, packet);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
      if (result < 0) {
        setError(error, "FLAC encoding failed: " + ffmpegError(result));
        return false;
      }
      av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
      packet->stream_index = stream->index;
      const int writeResult = av_interleaved_write_frame(format, packet);
      if (writeResult < 0) {
        setError(error, "Could not write FLAC audio: " +
                            ffmpegError(writeResult));
        return false;
      }
    }
  }

  bool sendFrame(AVFrame* input, std::string* error) {
    int result = avcodec_send_frame(encoder, input);
    if (result == AVERROR(EAGAIN)) {
      if (!receivePackets(error)) return false;
      result = avcodec_send_frame(encoder, input);
    }
    if (result < 0 && result != AVERROR_EOF) {
      setError(error, "Could not submit FLAC audio: " + ffmpegError(result));
      return false;
    }
    return receivePackets(error);
  }

  bool encode(const float* interleaved, std::size_t frameCount,
              std::string* error) {
    if (frameCount == 0) return true;
    if (frameCount > static_cast<std::size_t>(
                         std::numeric_limits<int>::max())) {
      setError(error, "A FLAC encoder frame is too large.");
      return false;
    }
    av_frame_unref(frame);
    frame->format = encoder->sample_fmt;
    frame->sample_rate = encoder->sample_rate;
    frame->nb_samples = static_cast<int>(frameCount);
    frame->pts = nextPts;
    if (av_channel_layout_copy(&frame->ch_layout,
                               &encoder->ch_layout) < 0 ||
        av_frame_get_buffer(frame, 0) < 0) {
      setError(error, "Could not allocate a FLAC encoder frame.");
      return false;
    }
    auto* destination = reinterpret_cast<std::int32_t*>(frame->data[0]);
    const std::size_t sampleCount = frameCount * channels;
    constexpr double kScale = 2147483647.0;
    for (std::size_t index = 0; index < sampleCount; ++index) {
      const float finite =
          std::isfinite(interleaved[index]) ? interleaved[index] : 0.0f;
      const double clamped = std::clamp(static_cast<double>(finite), -1.0,
                                        1.0);
      destination[index] = static_cast<std::int32_t>(
          std::llround(clamped * kScale));
    }
    nextPts += static_cast<std::int64_t>(frameCount);
    return sendFrame(frame, error);
  }

  bool drainCompleteFrames(std::string* error) {
    const std::size_t encoderFrames =
        encoder->frame_size > 0
            ? static_cast<std::size_t>(encoder->frame_size)
            : 4096u;
    while ((pending.size() - pendingOffset) / channels >= encoderFrames) {
      if (!encode(pending.data() + pendingOffset, encoderFrames, error)) {
        return false;
      }
      pendingOffset += encoderFrames * channels;
    }
    if (pendingOffset == pending.size()) {
      pending.clear();
      pendingOffset = 0;
    } else if (pendingOffset > 4 * encoderFrames * channels) {
      pending.erase(pending.begin(),
                    pending.begin() +
                        static_cast<std::ptrdiff_t>(pendingOffset));
      pendingOffset = 0;
    }
    return true;
  }
};

FlacWriter::FlacWriter() = default;
FlacWriter::~FlacWriter() = default;

bool FlacWriter::open(const std::filesystem::path& path,
                      std::uint32_t sampleRate, std::uint32_t channels,
                      std::string* error) {
  if (error) error->clear();
  if (path.empty() || sampleRate == 0 || sampleRate > 655350 ||
      channels == 0 || channels > 8) {
    setError(error, "The FLAC output configuration is invalid.");
    return false;
  }
  auto implementation = std::make_unique<Impl>();
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_alloc_output_context2(
      &implementation->format, nullptr, "flac", pathUtf8.c_str());
  if (result < 0 || !implementation->format) {
    setError(error, "Could not create a FLAC output: " +
                        ffmpegError(result));
    return false;
  }
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
  if (!codec) {
    setError(error, "This Radioify build has no FLAC encoder.");
    return false;
  }
  implementation->encoder = avcodec_alloc_context3(codec);
  implementation->stream =
      avformat_new_stream(implementation->format, nullptr);
  implementation->frame = av_frame_alloc();
  implementation->packet = av_packet_alloc();
  if (!implementation->encoder || !implementation->stream ||
      !implementation->frame || !implementation->packet) {
    setError(error, "Could not allocate the FLAC encoder.");
    return false;
  }
  implementation->channels = channels;
  implementation->encoder->sample_rate = static_cast<int>(sampleRate);
  implementation->encoder->sample_fmt = AV_SAMPLE_FMT_S32;
  implementation->encoder->time_base =
      AVRational{1, static_cast<int>(sampleRate)};
  implementation->encoder->bits_per_raw_sample = 24;
  av_channel_layout_default(&implementation->encoder->ch_layout,
                            static_cast<int>(channels));
  if (implementation->format->oformat->flags & AVFMT_GLOBALHEADER) {
    implementation->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  result = avcodec_open2(implementation->encoder, codec, nullptr);
  if (result < 0) {
    setError(error, "Could not open the FLAC encoder: " +
                        ffmpegError(result));
    return false;
  }
  implementation->stream->time_base = implementation->encoder->time_base;
  result = avcodec_parameters_from_context(
      implementation->stream->codecpar, implementation->encoder);
  if (result < 0) {
    setError(error, "Could not configure the FLAC stream: " +
                        ffmpegError(result));
    return false;
  }
  if (!(implementation->format->oformat->flags & AVFMT_NOFILE)) {
    result = avio_open(&implementation->format->pb, pathUtf8.c_str(),
                       AVIO_FLAG_WRITE);
    if (result < 0) {
      setError(error, "Could not create the FLAC file: " +
                          ffmpegError(result));
      return false;
    }
  }
  result = avformat_write_header(implementation->format, nullptr);
  if (result < 0) {
    setError(error, "Could not write the FLAC header: " +
                        ffmpegError(result));
    return false;
  }
  implementation->headerWritten = true;
  impl_ = std::move(implementation);
  return true;
}

bool FlacWriter::writeFrames(const float* interleaved,
                             std::size_t frameCount, std::string* error) {
  if (!impl_ || impl_->finished || (!interleaved && frameCount > 0)) {
    setError(error, "The FLAC output is not writable.");
    return false;
  }
  if (frameCount == 0) return true;
  const std::size_t sampleCount = frameCount * impl_->channels;
  impl_->pending.insert(impl_->pending.end(), interleaved,
                        interleaved + sampleCount);
  return impl_->drainCompleteFrames(error);
}

bool FlacWriter::finish(std::string* error) {
  if (!impl_ || impl_->finished) {
    setError(error, "The FLAC output is not open.");
    return false;
  }
  const std::size_t remainingFrames =
      (impl_->pending.size() - impl_->pendingOffset) / impl_->channels;
  if (remainingFrames > 0 &&
      !impl_->encode(impl_->pending.data() + impl_->pendingOffset,
                     remainingFrames, error)) {
    return false;
  }
  impl_->pending.clear();
  impl_->pendingOffset = 0;
  if (!impl_->sendFrame(nullptr, error)) return false;
  const int result = av_write_trailer(impl_->format);
  if (result < 0) {
    setError(error, "Could not finalize the FLAC file: " +
                        ffmpegError(result));
    return false;
  }
  if (impl_->format->pb && !(impl_->format->oformat->flags & AVFMT_NOFILE)) {
    const int closeResult = avio_closep(&impl_->format->pb);
    if (closeResult < 0) {
      setError(error, "Could not close the FLAC file: " +
                          ffmpegError(closeResult));
      return false;
    }
  }
  impl_->finished = true;
  return true;
}

}  // namespace audio_file
