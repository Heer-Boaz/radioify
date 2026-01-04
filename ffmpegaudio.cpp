#include "ffmpegaudio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

constexpr int64_t kProbeSize = 64 * 1024;
constexpr int64_t kAnalyzeDurationFastUs = 500000;
constexpr int64_t kAnalyzeDurationFallbackUs = 5000000;

bool openInputWithProbe(const std::filesystem::path& path,
                        int64_t analyzeDurationUs,
                        AVFormatContext** outFmt,
                        std::string* error) {
  AVDictionary* options = nullptr;
  av_dict_set_int(&options, "probesize", kProbeSize, 0);
  av_dict_set_int(&options, "analyzeduration", analyzeDurationUs, 0);

  int openErr =
      avformat_open_input(outFmt, path.string().c_str(), nullptr, &options);
  av_dict_free(&options);
  if (openErr < 0) {
    std::string msg = "Failed to open audio: " + ffmpegError(openErr);
    setError(error, msg.c_str());
    return false;
  }

  (*outFmt)->flags |= AVFMT_FLAG_NOBUFFER;
  (*outFmt)->max_analyze_duration = analyzeDurationUs;
  (*outFmt)->probesize = kProbeSize;
  return true;
}

int64_t rescaleToFrames(int64_t value, AVRational src, uint32_t sampleRate) {
  if (value <= 0 || sampleRate == 0) return 0;
  AVRational dst{1, static_cast<int>(sampleRate)};
  return av_rescale_q(value, src, dst);
}

uint64_t rescaleFrames(uint64_t value, uint32_t inRate, uint32_t outRate) {
  if (value == 0 || inRate == 0 || outRate == 0 || inRate == outRate) {
    return value;
  }
  AVRational src{1, static_cast<int>(inRate)};
  AVRational dst{1, static_cast<int>(outRate)};
  return static_cast<uint64_t>(
      av_rescale_q(static_cast<int64_t>(value), src, dst));
}

int64_t rescaleUsToFrames(int64_t value, uint32_t sampleRate) {
  if (value == 0 || sampleRate == 0) return 0;
  AVRational src{1, AV_TIME_BASE};
  AVRational dst{1, static_cast<int>(sampleRate)};
  return av_rescale_q(value, src, dst);
}
}  // namespace

struct FfmpegAudioDecoder::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  SwrContext* swr = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  int streamIndex = -1;
  AVRational timeBase{0, 1};
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint32_t inSampleRate = 0;
  uint64_t totalFrames = 0;
  uint64_t rawTotalFrames = 0;
  uint64_t initialPaddingFrames = 0;
  uint64_t trailingPaddingFrames = 0;
  int64_t formatStartUs = 0;
  int64_t streamStartUs = 0;
  int64_t startOffsetFrames = 0;
  bool eof = false;
  bool atEnd = false;
  std::vector<float> cache;
  size_t cachePos = 0;
  std::vector<float> convertBuffer;
};

FfmpegAudioDecoder::FfmpegAudioDecoder() = default;
FfmpegAudioDecoder::~FfmpegAudioDecoder() { uninit(); }

bool FfmpegAudioDecoder::init(const std::filesystem::path& path,
                              uint32_t channels, uint32_t sampleRate,
                              std::string* error) {
  uninit();

  AVFormatContext* fmt = nullptr;
  if (!openInputWithProbe(path, kAnalyzeDurationFastUs, &fmt, error)) {
    return false;
  }

  int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    if (!openInputWithProbe(path, kAnalyzeDurationFallbackUs, &fmt, error)) {
      return false;
    }
    infoErr = avformat_find_stream_info(fmt, nullptr);
    if (infoErr < 0) {
      std::string msg = "Failed to read audio stream info: " +
                        ffmpegError(infoErr);
      avformat_close_input(&fmt);
      setError(error, msg.c_str());
      return false;
    }
  }

  const AVCodec* codec = nullptr;
  int streamIndex =
      av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (streamIndex < 0 || !codec) {
    avformat_close_input(&fmt);
    setError(error, "No audio stream found.");
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    avformat_close_input(&fmt);
    setError(error, "Failed to allocate audio decoder.");
    return false;
  }
  if (avcodec_parameters_to_context(ctx,
                                    fmt->streams[streamIndex]->codecpar) < 0) {
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to configure audio decoder.");
    return false;
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to open audio decoder.");
    return false;
  }

  AVCodecParameters* par = fmt->streams[streamIndex]->codecpar;
  int64_t formatStartUs =
      (fmt->start_time != AV_NOPTS_VALUE) ? fmt->start_time : 0;
  int64_t streamStartUs = 0;
  if (fmt->streams[streamIndex]->start_time != AV_NOPTS_VALUE) {
    streamStartUs = av_rescale_q(fmt->streams[streamIndex]->start_time,
                                 fmt->streams[streamIndex]->time_base,
                                 AVRational{1, AV_TIME_BASE});
  }

  SwrContext* swr = nullptr;
  AVChannelLayout inLayout{};
  int inChannels = ctx->ch_layout.nb_channels;
  if (inChannels <= 0 && par) {
    inChannels = par->ch_layout.nb_channels;
  }
  if (inChannels <= 0) {
    inChannels = 1;
  }
  if (ctx->ch_layout.nb_channels > 0) {
    av_channel_layout_copy(&inLayout, &ctx->ch_layout);
  } else {
    av_channel_layout_default(&inLayout, inChannels);
  }
  AVChannelLayout outLayout{};
  av_channel_layout_default(&outLayout, static_cast<int>(channels));

  if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT,
                          static_cast<int>(sampleRate), &inLayout,
                          ctx->sample_fmt, ctx->sample_rate, 0,
                          nullptr) < 0) {
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to configure audio resampler.");
    return false;
  }
  if (swr_init(swr) < 0) {
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to initialize audio resampler.");
    return false;
  }
  av_channel_layout_uninit(&inLayout);
  av_channel_layout_uninit(&outLayout);

  AVFrame* frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !packet) {
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to allocate audio buffers.");
    return false;
  }

  Impl* impl = new Impl();
  impl->fmt = fmt;
  impl->codec = ctx;
  impl->swr = swr;
  impl->frame = frame;
  impl->packet = packet;
  impl->streamIndex = streamIndex;
  impl->timeBase = fmt->streams[streamIndex]->time_base;
  impl->channels = channels;
  impl->sampleRate = sampleRate;
  impl->inSampleRate = ctx->sample_rate > 0
                           ? static_cast<uint32_t>(ctx->sample_rate)
                           : sampleRate;
  impl->formatStartUs = formatStartUs;
  impl->streamStartUs = streamStartUs;
  impl->startOffsetFrames =
      rescaleUsToFrames(streamStartUs - formatStartUs, sampleRate);
  if (par) {
    if (par->initial_padding > 0) {
      impl->initialPaddingFrames = rescaleFrames(
          static_cast<uint64_t>(par->initial_padding), impl->inSampleRate,
          sampleRate);
    }
    if (par->trailing_padding > 0) {
      impl->trailingPaddingFrames = rescaleFrames(
          static_cast<uint64_t>(par->trailing_padding), impl->inSampleRate,
          sampleRate);
    }
  }
  if (fmt->streams[streamIndex]->duration > 0) {
    impl->rawTotalFrames = static_cast<uint64_t>(
        rescaleToFrames(fmt->streams[streamIndex]->duration,
                        fmt->streams[streamIndex]->time_base, sampleRate));
  } else if (fmt->duration > 0) {
    AVRational tb{1, AV_TIME_BASE};
    impl->rawTotalFrames =
        static_cast<uint64_t>(rescaleToFrames(fmt->duration, tb, sampleRate));
  }
  if (impl->rawTotalFrames > 0) {
    uint64_t pad = impl->initialPaddingFrames + impl->trailingPaddingFrames;
    if (impl->rawTotalFrames > pad) {
      impl->totalFrames = impl->rawTotalFrames - pad;
    } else {
      impl->totalFrames = 0;
    }
  }
  impl_ = impl;
  return true;
}

void FfmpegAudioDecoder::uninit() {
  if (!impl_) return;
  av_packet_free(&impl_->packet);
  av_frame_free(&impl_->frame);
  if (impl_->swr) {
    swr_free(&impl_->swr);
  }
  if (impl_->codec) {
    avcodec_free_context(&impl_->codec);
  }
  if (impl_->fmt) {
    avformat_close_input(&impl_->fmt);
  }
  delete impl_;
  impl_ = nullptr;
}

bool FfmpegAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames) return false;
  *outFrames = impl_ ? impl_->totalFrames : 0;
  return *outFrames > 0;
}

bool FfmpegAudioDecoder::getStartOffsetFrames(int64_t* outFrames) const {
  if (!outFrames) return false;
  *outFrames = impl_ ? impl_->startOffsetFrames : 0;
  return true;
}

bool FfmpegAudioDecoder::getPaddingFrames(uint64_t* outInitial,
                                          uint64_t* outTrailing) const {
  if (!outInitial && !outTrailing) return false;
  if (outInitial) {
    *outInitial = impl_ ? impl_->initialPaddingFrames : 0;
  }
  if (outTrailing) {
    *outTrailing = impl_ ? impl_->trailingPaddingFrames : 0;
  }
  return true;
}

bool FfmpegAudioDecoder::seekToFrame(uint64_t frame) {
  if (!impl_ || !impl_->fmt || !impl_->codec) return false;
  if (impl_->streamIndex < 0) return false;

  if (impl_->rawTotalFrames > 0 && frame > impl_->rawTotalFrames) {
    frame = impl_->rawTotalFrames;
  }
  AVRational src{1, static_cast<int>(impl_->sampleRate)};
  int64_t target = av_rescale_q(static_cast<int64_t>(frame), src,
                                impl_->timeBase);
  int res =
      av_seek_frame(impl_->fmt, impl_->streamIndex, target, AVSEEK_FLAG_BACKWARD);
  if (res < 0) return false;

  avcodec_flush_buffers(impl_->codec);
  if (impl_->swr) {
    swr_close(impl_->swr);
    if (swr_init(impl_->swr) < 0) {
      return false;
    }
  }
  impl_->cache.clear();
  impl_->cachePos = 0;
  impl_->eof = false;
  impl_->atEnd = false;
  return true;
}

bool FfmpegAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                    uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!impl_ || !impl_->codec || !out) return false;
  if (frameCount == 0) return true;

  uint64_t totalRead = 0;
  const uint32_t channels = impl_->channels;

  auto drainCache = [&]() {
    if (impl_->cachePos >= impl_->cache.size()) {
      impl_->cache.clear();
      impl_->cachePos = 0;
      return;
    }
    if (impl_->cachePos > 0) {
      impl_->cache.erase(impl_->cache.begin(),
                         impl_->cache.begin() +
                             static_cast<ptrdiff_t>(impl_->cachePos));
      impl_->cachePos = 0;
    }
  };

  while (totalRead < frameCount) {
    size_t availableSamples =
        (impl_->cache.size() > impl_->cachePos)
            ? (impl_->cache.size() - impl_->cachePos)
            : 0;
    if (availableSamples > 0) {
      size_t availableFrames = availableSamples / channels;
      size_t framesToCopy =
          std::min<size_t>(frameCount - totalRead, availableFrames);
      size_t samplesToCopy = framesToCopy * channels;
      std::memcpy(out + totalRead * channels,
                  impl_->cache.data() + impl_->cachePos,
                  samplesToCopy * sizeof(float));
      impl_->cachePos += samplesToCopy;
      totalRead += framesToCopy;
      if (totalRead >= frameCount) break;
      drainCache();
    }

    if (impl_->atEnd) break;

    bool gotFrame = false;
    while (!gotFrame) {
      int recv = avcodec_receive_frame(impl_->codec, impl_->frame);
      if (recv == 0) {
        gotFrame = true;
        int outSamples =
            swr_get_out_samples(impl_->swr, impl_->frame->nb_samples);
        if (outSamples <= 0) {
          break;
        }
        size_t needed = static_cast<size_t>(outSamples) * channels;
        if (impl_->convertBuffer.size() < needed) {
          impl_->convertBuffer.resize(needed);
        }
        uint8_t* outData[1] = {
            reinterpret_cast<uint8_t*>(impl_->convertBuffer.data())};
        int converted = swr_convert(
            impl_->swr, outData, outSamples,
            const_cast<const uint8_t**>(impl_->frame->data),
            impl_->frame->nb_samples);
        if (converted < 0) {
          impl_->atEnd = true;
          break;
        }
        size_t samples = static_cast<size_t>(converted) * channels;
        if (samples == 0) {
          break;
        }
        drainCache();
        impl_->cache.insert(impl_->cache.end(), impl_->convertBuffer.begin(),
                            impl_->convertBuffer.begin() +
                                static_cast<ptrdiff_t>(samples));
        break;
      }
      if (recv == AVERROR_EOF) {
        impl_->atEnd = true;
        break;
      }
      if (recv != AVERROR(EAGAIN)) {
        impl_->atEnd = true;
        return false;
      }

      if (!impl_->eof) {
        int read = av_read_frame(impl_->fmt, impl_->packet);
        if (read < 0) {
          impl_->eof = true;
          avcodec_send_packet(impl_->codec, nullptr);
          continue;
        }
        if (impl_->packet->stream_index != impl_->streamIndex) {
          av_packet_unref(impl_->packet);
          continue;
        }
        int send = avcodec_send_packet(impl_->codec, impl_->packet);
        av_packet_unref(impl_->packet);
        if (send < 0) {
          impl_->atEnd = true;
          return false;
        }
      } else {
        avcodec_send_packet(impl_->codec, nullptr);
      }
    }
  }

  if (framesRead) *framesRead = totalRead;
  return true;
}
