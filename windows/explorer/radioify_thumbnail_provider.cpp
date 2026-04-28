#include "radioify_thumbnail_provider.h"

#include "radioify_explorer_module.h"

#include <shlguid.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr AVRational kAvTimeBaseQ{1, AV_TIME_BASE};

struct AvCodecContextHandle {
  ~AvCodecContextHandle() {
    if (ptr) {
      avcodec_free_context(&ptr);
    }
  }

  AVCodecContext** put() { return &ptr; }
  AVCodecContext* get() const { return ptr; }
  AVCodecContext* operator->() const { return ptr; }

  AVCodecContext* ptr = nullptr;
};

struct AvFrameHandle {
  AvFrameHandle() : ptr(av_frame_alloc()) {}
  ~AvFrameHandle() {
    if (ptr) {
      av_frame_free(&ptr);
    }
  }

  AVFrame* get() const { return ptr; }
  AVFrame* operator->() const { return ptr; }

  AVFrame* ptr = nullptr;
};

struct AvPacketHandle {
  AvPacketHandle() : ptr(av_packet_alloc()) {}
  ~AvPacketHandle() {
    if (ptr) {
      av_packet_free(&ptr);
    }
  }

  AVPacket* get() const { return ptr; }
  AVPacket* operator->() const { return ptr; }

  AVPacket* ptr = nullptr;
};

struct SwsContextHandle {
  ~SwsContextHandle() {
    if (ptr) {
      sws_freeContext(ptr);
    }
  }

  SwsContext* ptr = nullptr;
};

struct AvInputSource {
  ComPtr<IStream> stream;
  int64_t size = 0;
};

struct AvIoContextHandle {
  ~AvIoContextHandle() {
    if (ptr) {
      av_freep(&ptr->buffer);
      avio_context_free(&ptr);
    }
  }

  AVIOContext* ptr = nullptr;
};

struct AvFormatInput {
  ~AvFormatInput() {
    if (ptr) {
      avformat_close_input(&ptr);
    }
  }

  AVFormatContext* get() const { return ptr; }
  AVFormatContext* operator->() const { return ptr; }

  AVFormatContext* ptr = nullptr;
  AvInputSource source;
  AvIoContextHandle io;
};

HRESULT convertWicSourceToBitmap(IWICBitmapSource* bitmapSource,
                                 IWICImagingFactory* factory,
                                 HBITMAP* outBitmap,
                                 WTS_ALPHATYPE* outAlpha) {
  if (!bitmapSource || !factory || !outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  ComPtr<IWICBitmapSource> converted;
  WICPixelFormatGUID sourceFormat{};
  HRESULT hr = bitmapSource->GetPixelFormat(&sourceFormat);
  if (FAILED(hr)) return hr;

  if (IsEqualGUID(sourceFormat, GUID_WICPixelFormat32bppBGRA)) {
    hr = bitmapSource->QueryInterface(IID_PPV_ARGS(converted.put()));
  } else {
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (SUCCEEDED(hr)) {
      hr = converter->Initialize(bitmapSource, GUID_WICPixelFormat32bppBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
      hr = converter->QueryInterface(IID_PPV_ARGS(converted.put()));
    }
  }
  if (FAILED(hr)) return hr;

  UINT width = 0;
  UINT height = 0;
  hr = converted->GetSize(&width, &height);
  if (FAILED(hr)) return hr;
  if (width == 0 || height == 0) return E_FAIL;
  if (width > static_cast<UINT>(INT32_MAX) ||
      height > static_cast<UINT>(INT32_MAX)) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  const uint64_t stride64 = static_cast<uint64_t>(width) * 4u;
  const uint64_t bytes64 = stride64 * static_cast<uint64_t>(height);
  if (stride64 > UINT32_MAX || bytes64 > UINT32_MAX) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  BITMAPINFO bitmapInfo{};
  bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
  bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
  bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) return E_OUTOFMEMORY;

  WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  hr = converted->CopyPixels(&rect, static_cast<UINT>(stride64),
                             static_cast<UINT>(bytes64),
                             static_cast<BYTE*>(bits));
  if (FAILED(hr)) {
    DeleteObject(bitmap);
    return hr;
  }

  *outBitmap = bitmap;
  *outAlpha = WTSAT_ARGB;
  return S_OK;
}

HRESULT loadWicDecoderThumbnailBitmap(IWICImagingFactory* factory,
                                      IWICBitmapDecoder* decoder,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!factory || !decoder || !outBitmap || !outAlpha) return E_POINTER;
  if (requestedSize == 0) return E_INVALIDARG;

  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(0, frame.put());
  if (FAILED(hr)) return hr;

  UINT sourceWidth = 0;
  UINT sourceHeight = 0;
  hr = frame->GetSize(&sourceWidth, &sourceHeight);
  if (FAILED(hr)) return hr;
  if (sourceWidth == 0 || sourceHeight == 0) return E_FAIL;

  IWICBitmapSource* source = frame.get();
  ComPtr<IWICBitmapScaler> scaler;
  const UINT maxDimension = sourceWidth > sourceHeight ? sourceWidth : sourceHeight;
  if (maxDimension > requestedSize) {
    const uint64_t targetWidth64 =
        (static_cast<uint64_t>(sourceWidth) * requestedSize + maxDimension / 2) /
        maxDimension;
    const uint64_t targetHeight64 =
        (static_cast<uint64_t>(sourceHeight) * requestedSize + maxDimension / 2) /
        maxDimension;
    const UINT targetWidth = static_cast<UINT>(targetWidth64 > 0 ? targetWidth64 : 1);
    const UINT targetHeight =
        static_cast<UINT>(targetHeight64 > 0 ? targetHeight64 : 1);

    hr = factory->CreateBitmapScaler(scaler.put());
    if (SUCCEEDED(hr)) {
      hr = scaler->Initialize(frame.get(), targetWidth, targetHeight,
                              WICBitmapInterpolationModeFant);
    }
    if (FAILED(hr)) return hr;
    source = scaler.get();
  }

  return convertWicSourceToBitmap(source, factory, outBitmap, outAlpha);
}

HRESULT loadWicThumbnailBitmapFromMemory(const uint8_t* bytes,
                                         size_t byteCount,
                                         UINT requestedSize,
                                         HBITMAP* outBitmap,
                                         WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (!bytes || byteCount == 0 || requestedSize == 0) return E_INVALIDARG;
  if (byteCount > std::numeric_limits<DWORD>::max()) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(stream.put());
  if (FAILED(hr)) return hr;

  hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes),
                                    static_cast<DWORD>(byteCount));
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromStream(
      stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
  if (FAILED(hr)) return hr;

  return loadWicDecoderThumbnailBitmap(factory.get(), decoder.get(),
                                       requestedSize, outBitmap, outAlpha);
}

HRESULT seekStreamStart(IStream* stream) {
  if (!stream) return E_POINTER;
  LARGE_INTEGER offset{};
  return stream->Seek(offset, STREAM_SEEK_SET, nullptr);
}

HRESULT loadWicThumbnailBitmapFromStream(IStream* stream,
                                         UINT requestedSize,
                                         HBITMAP* outBitmap,
                                         WTS_ALPHATYPE* outAlpha) {
  if (!stream || !outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (requestedSize == 0) return E_INVALIDARG;

  HRESULT hr = seekStreamStart(stream);
  if (FAILED(hr)) return hr;

  ComPtr<IWICImagingFactory> factory;
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(factory.put()));
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromStream(
      stream, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
  if (FAILED(hr)) return hr;

  return loadWicDecoderThumbnailBitmap(factory.get(), decoder.get(),
                                       requestedSize, outBitmap, outAlpha);
}

HRESULT streamSize(IStream* stream, int64_t* outSize) {
  if (!stream || !outSize) return E_POINTER;
  *outSize = 0;

  STATSTG stat{};
  HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
  if (SUCCEEDED(hr)) {
    if (stat.cbSize.QuadPart >
        static_cast<ULONGLONG>(std::numeric_limits<int64_t>::max())) {
      return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }
    *outSize = static_cast<int64_t>(stat.cbSize.QuadPart);
    return S_OK;
  }

  LARGE_INTEGER zero{};
  ULARGE_INTEGER original{};
  hr = stream->Seek(zero, STREAM_SEEK_CUR, &original);
  if (FAILED(hr)) return hr;

  ULARGE_INTEGER end{};
  hr = stream->Seek(zero, STREAM_SEEK_END, &end);
  if (SUCCEEDED(hr)) {
    LARGE_INTEGER restore{};
    restore.QuadPart = static_cast<LONGLONG>(original.QuadPart);
    const HRESULT restoreHr = stream->Seek(restore, STREAM_SEEK_SET, nullptr);
    if (FAILED(restoreHr)) return restoreHr;
    if (end.QuadPart >
        static_cast<ULONGLONG>(std::numeric_limits<int64_t>::max())) {
      return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }
    *outSize = static_cast<int64_t>(end.QuadPart);
  }
  return hr;
}

HRESULT openStreamAvSource(IStream* mediaStream, AvInputSource* source) {
  if (!mediaStream || !source) return E_POINTER;

  HRESULT hr = mediaStream->QueryInterface(IID_PPV_ARGS(source->stream.put()));
  if (FAILED(hr)) return hr;

  hr = streamSize(source->stream.get(), &source->size);
  if (FAILED(hr)) return hr;
  if (source->size <= 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);

  hr = seekStreamStart(source->stream.get());
  if (FAILED(hr)) return hr;

  return S_OK;
}

int readAvSource(void* opaque, uint8_t* buffer, int bufferSize) {
  if (!opaque || !buffer || bufferSize < 0) return AVERROR(EINVAL);
  if (bufferSize == 0) return 0;

  AvInputSource* source = static_cast<AvInputSource*>(opaque);
  ULONG bytesRead = 0;
  const HRESULT hr = source->stream->Read(buffer,
                                         static_cast<ULONG>(bufferSize),
                                         &bytesRead);
  if (FAILED(hr)) return AVERROR(EIO);
  if (bytesRead == 0) return AVERROR_EOF;
  return static_cast<int>(bytesRead);
}

int64_t seekAvSource(void* opaque, int64_t offset, int whence) {
  if (!opaque) return AVERROR(EINVAL);

  AvInputSource* source = static_cast<AvInputSource*>(opaque);
  if (whence == AVSEEK_SIZE) return source->size;

  DWORD streamOrigin = STREAM_SEEK_SET;
  switch (whence & 0xff) {
    case SEEK_SET:
      streamOrigin = STREAM_SEEK_SET;
      break;
    case SEEK_CUR:
      streamOrigin = STREAM_SEEK_CUR;
      break;
    case SEEK_END:
      streamOrigin = STREAM_SEEK_END;
      break;
    default:
      return AVERROR(EINVAL);
  }

  LARGE_INTEGER distance{};
  distance.QuadPart = offset;
  ULARGE_INTEGER position{};
  const HRESULT hr = source->stream->Seek(distance, streamOrigin, &position);
  if (FAILED(hr)) return AVERROR(EIO);
  return static_cast<int64_t>(position.QuadPart);
}

HRESULT openMediaInput(IStream* mediaStream, AvFormatInput* input) {
  if (!input) return E_POINTER;

  HRESULT hr = openStreamAvSource(mediaStream, &input->source);
  if (FAILED(hr)) return hr;

  input->ptr = avformat_alloc_context();
  if (!input->ptr) return E_OUTOFMEMORY;

  constexpr int kAvIoBufferSize = 64 * 1024;
  unsigned char* ioBuffer =
      static_cast<unsigned char*>(av_malloc(kAvIoBufferSize));
  if (!ioBuffer) return E_OUTOFMEMORY;

  input->io.ptr =
      avio_alloc_context(ioBuffer, kAvIoBufferSize, 0, &input->source,
                         readAvSource, nullptr, seekAvSource);
  if (!input->io.ptr) {
    av_free(ioBuffer);
    return E_OUTOFMEMORY;
  }

  input->ptr->pb = input->io.ptr;
  input->ptr->flags |= AVFMT_FLAG_CUSTOM_IO;

  AVFormatContext* format = input->ptr;
  const int openErr = avformat_open_input(&format, nullptr, nullptr, nullptr);
  input->ptr = format;
  if (openErr < 0) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);

  const int infoErr = avformat_find_stream_info(input->get(), nullptr);
  if (infoErr < 0) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
  return S_OK;
}

HRESULT loadEmbeddedArtworkBitmap(IStream* mediaStream,
                                  UINT requestedSize,
                                  HBITMAP* outBitmap,
                                  WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  AvFormatInput input;
  HRESULT hr = openMediaInput(mediaStream, &input);
  if (FAILED(hr)) return hr;

  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    AVStream* stream = input->streams[i];
    if (!stream || !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      continue;
    }
    const AVPacket& pic = stream->attached_pic;
    if (!pic.data || pic.size <= 0) {
      continue;
    }
    hr = loadWicThumbnailBitmapFromMemory(
        pic.data, static_cast<size_t>(pic.size), requestedSize, outBitmap,
        outAlpha);
    if (SUCCEEDED(hr)) return hr;
  }
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

int selectThumbnailVideoStream(AVFormatContext* format,
                               const AVCodec** outCodec) {
  if (!format || !outCodec) return -1;
  *outCodec = nullptr;

  int bestIndex = -1;
  int64_t bestPixels = -1;
  bool bestDefault = false;
  const AVCodec* bestCodec = nullptr;

  for (unsigned int i = 0; i < format->nb_streams; ++i) {
    AVStream* stream = format->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
    if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) continue;

    const int64_t pixels = static_cast<int64_t>(stream->codecpar->width) *
                           static_cast<int64_t>(stream->codecpar->height);
    const bool isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
    if (pixels > bestPixels || (pixels == bestPixels && isDefault && !bestDefault)) {
      bestIndex = static_cast<int>(i);
      bestPixels = pixels;
      bestDefault = isDefault;
      bestCodec = codec;
    }
  }

  *outCodec = bestCodec;
  return bestIndex;
}

HRESULT copyVideoFrameToBitmap(const AVFrame* frame,
                               UINT requestedSize,
                               HBITMAP* outBitmap,
                               WTS_ALPHATYPE* outAlpha) {
  if (!frame || !outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;
  if (requestedSize == 0 || frame->width <= 0 || frame->height <= 0) {
    return E_INVALIDARG;
  }
  if (requestedSize > static_cast<UINT>(std::numeric_limits<int>::max())) {
    return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
  }

  const int maxDimension = std::max(frame->width, frame->height);
  const int targetWidth = std::max(
      1, static_cast<int>((static_cast<int64_t>(frame->width) * requestedSize +
                           maxDimension / 2) /
                          maxDimension));
  const int targetHeight = std::max(
      1, static_cast<int>((static_cast<int64_t>(frame->height) * requestedSize +
                           maxDimension / 2) /
                          maxDimension));

  BITMAPINFO bitmapInfo{};
  bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
  bitmapInfo.bmiHeader.biWidth = targetWidth;
  bitmapInfo.bmiHeader.biHeight = -targetHeight;
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) return E_OUTOFMEMORY;

  SwsContextHandle scale;
  scale.ptr = sws_getContext(frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format),
                             targetWidth, targetHeight, AV_PIX_FMT_BGRA,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!scale.ptr) {
    DeleteObject(bitmap);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  uint8_t* dstData[4] = {static_cast<uint8_t*>(bits), nullptr, nullptr,
                         nullptr};
  int dstLinesize[4] = {targetWidth * 4, 0, 0, 0};
  const int scaledRows = sws_scale(scale.ptr, frame->data, frame->linesize, 0,
                                  frame->height, dstData, dstLinesize);
  if (scaledRows != targetHeight) {
    DeleteObject(bitmap);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  uint8_t* pixels = static_cast<uint8_t*>(bits);
  for (int y = 0; y < targetHeight; ++y) {
    uint8_t* row = pixels + static_cast<size_t>(y) * dstLinesize[0];
    for (int x = 0; x < targetWidth; ++x) {
      row[x * 4 + 3] = 0xff;
    }
  }

  *outBitmap = bitmap;
  *outAlpha = WTSAT_ARGB;
  return S_OK;
}

HRESULT sendPacketAndReceiveThumbnail(AVCodecContext* codec,
                                      const AVPacket* packet,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!codec || !outBitmap || !outAlpha) return E_POINTER;
  const int sendErr = avcodec_send_packet(codec, packet);
  if (sendErr < 0 && sendErr != AVERROR_EOF && sendErr != AVERROR(EAGAIN)) {
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }

  AvFrameHandle frame;
  if (!frame.get()) return E_OUTOFMEMORY;

  for (;;) {
    const int receiveErr = avcodec_receive_frame(codec, frame.get());
    if (receiveErr == AVERROR(EAGAIN) || receiveErr == AVERROR_EOF) {
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (receiveErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

    HRESULT hr =
        copyVideoFrameToBitmap(frame.get(), requestedSize, outBitmap, outAlpha);
    av_frame_unref(frame.get());
    if (SUCCEEDED(hr)) return hr;
  }
}

HRESULT decodeVideoThumbnailFromCurrentPosition(AVFormatContext* format,
                                                AVCodecContext* codec,
                                                int streamIndex,
                                                UINT requestedSize,
                                                HBITMAP* outBitmap,
                                                WTS_ALPHATYPE* outAlpha) {
  if (!format || !codec || streamIndex < 0 || !outBitmap || !outAlpha) {
    return E_POINTER;
  }

  AvPacketHandle packet;
  if (!packet.get()) return E_OUTOFMEMORY;

  constexpr int kMaxThumbnailPackets = 4096;
  constexpr int kMaxThumbnailVideoPackets = 192;
  int videoPacketCount = 0;
  for (int packetCount = 0; packetCount < kMaxThumbnailPackets; ++packetCount) {
    const int readErr = av_read_frame(format, packet.get());
    if (readErr < 0) break;

    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (packet->stream_index == streamIndex) {
      ++videoPacketCount;
      hr = sendPacketAndReceiveThumbnail(codec, packet.get(), requestedSize,
                                         outBitmap, outAlpha);
    }
    av_packet_unref(packet.get());
    if (SUCCEEDED(hr)) return hr;
    if (videoPacketCount >= kMaxThumbnailVideoPackets) break;
  }

  return sendPacketAndReceiveThumbnail(codec, nullptr, requestedSize, outBitmap,
                                       outAlpha);
}

HRESULT primeVideoDecoderFromCurrentPosition(AVFormatContext* format,
                                             AVCodecContext* codec,
                                             int streamIndex) {
  if (!format || !codec || streamIndex < 0) return E_POINTER;

  AvPacketHandle packet;
  AvFrameHandle frame;
  if (!packet.get() || !frame.get()) return E_OUTOFMEMORY;

  constexpr int kMaxThumbnailPackets = 4096;
  constexpr int kMaxThumbnailVideoPackets = 192;
  int videoPacketCount = 0;
  for (int packetCount = 0; packetCount < kMaxThumbnailPackets; ++packetCount) {
    const int readErr = av_read_frame(format, packet.get());
    if (readErr < 0) break;

    if (packet->stream_index == streamIndex) {
      ++videoPacketCount;
      const int sendErr = avcodec_send_packet(codec, packet.get());
      if (sendErr < 0 && sendErr != AVERROR(EAGAIN) &&
          sendErr != AVERROR_EOF) {
        av_packet_unref(packet.get());
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
      }

      for (;;) {
        const int receiveErr = avcodec_receive_frame(codec, frame.get());
        if (receiveErr == 0) {
          av_frame_unref(frame.get());
          av_packet_unref(packet.get());
          return S_OK;
        }
        if (receiveErr == AVERROR(EAGAIN) || receiveErr == AVERROR_EOF) {
          break;
        }
        av_packet_unref(packet.get());
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
      }
    }

    av_packet_unref(packet.get());
    if (videoPacketCount >= kMaxThumbnailVideoPackets) break;
  }

  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

int64_t thumbnailSeekTimestamp(AVFormatContext* format, AVStream* stream) {
  if (!format || !stream) return AV_NOPTS_VALUE;

  if (stream->duration > 0) {
    const int64_t base =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    return base + stream->duration / 10;
  }
  if (format->duration > 0) {
    int64_t target = av_rescale_q(format->duration / 10, kAvTimeBaseQ,
                                  stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE) target += stream->start_time;
    return target;
  }
  return AV_NOPTS_VALUE;
}

HRESULT seekVideoThumbnailPosition(AVFormatContext* format,
                                   AVCodecContext* codec,
                                   int streamIndex,
                                   int64_t timestamp) {
  if (!format || !codec || streamIndex < 0) return E_POINTER;
  if (timestamp == AV_NOPTS_VALUE) return S_FALSE;

  const int seekErr =
      av_seek_frame(format, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
  if (seekErr < 0) return S_FALSE;
  avcodec_flush_buffers(codec);
  return S_OK;
}

HRESULT loadVideoFrameThumbnailBitmap(IStream* mediaStream,
                                      UINT requestedSize,
                                      HBITMAP* outBitmap,
                                      WTS_ALPHATYPE* outAlpha) {
  if (!outBitmap || !outAlpha) return E_POINTER;
  *outBitmap = nullptr;
  *outAlpha = WTSAT_UNKNOWN;

  AvFormatInput input;
  HRESULT hr = openMediaInput(mediaStream, &input);
  if (FAILED(hr)) return hr;

  const AVCodec* codec = nullptr;
  const int streamIndex = selectThumbnailVideoStream(input.get(), &codec);
  if (streamIndex < 0 || !codec) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  AVStream* stream = input->streams[streamIndex];
  AvCodecContextHandle codecContext;
  codecContext.ptr = avcodec_alloc_context3(codec);
  if (!codecContext.get()) return E_OUTOFMEMORY;

  int ffErr = avcodec_parameters_to_context(codecContext.get(), stream->codecpar);
  if (ffErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

  ffErr = avcodec_open2(codecContext.get(), codec, nullptr);
  if (ffErr < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

  primeVideoDecoderFromCurrentPosition(input.get(), codecContext.get(),
                                       streamIndex);

  const int64_t seekTimestamp = thumbnailSeekTimestamp(input.get(), stream);
  seekVideoThumbnailPosition(input.get(), codecContext.get(), streamIndex,
                             seekTimestamp);

  hr = decodeVideoThumbnailFromCurrentPosition(input.get(), codecContext.get(),
                                               streamIndex, requestedSize,
                                               outBitmap, outAlpha);
  if (SUCCEEDED(hr)) return hr;
  if (seekTimestamp == AV_NOPTS_VALUE) return hr;

  const int64_t startTimestamp =
      stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
  seekVideoThumbnailPosition(input.get(), codecContext.get(), streamIndex,
                             startTimestamp);
  return decodeVideoThumbnailFromCurrentPosition(input.get(), codecContext.get(),
                                                 streamIndex, requestedSize,
                                                 outBitmap, outAlpha);
}

class RadioifyThumbnailProvider final : public IThumbnailProvider,
                                        public IInitializeWithStream,
                                        public IInitializeWithFile,
                                        public IInitializeWithItem {
 public:
  RadioifyThumbnailProvider() { radioifyExplorerObjectCreated(); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) {
      *object = static_cast<IThumbnailProvider*>(this);
    } else if (riid == IID_IInitializeWithStream) {
      *object = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IInitializeWithFile) {
      *object = static_cast<IInitializeWithFile*>(this);
    } else if (riid == IID_IInitializeWithItem) {
      *object = static_cast<IInitializeWithItem*>(this);
    } else {
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP Initialize(IStream* stream, DWORD) override {
    try {
      return initializeFromStream(stream);
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP Initialize(LPCWSTR filePath, DWORD) override {
    try {
      if (!filePath || filePath[0] == L'\0') return E_INVALIDARG;
      if (mediaStream_) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
      }
      ComPtr<IStream> stream;
      HRESULT hr = SHCreateStreamOnFileEx(filePath,
                                          STGM_READ | STGM_SHARE_DENY_NONE,
                                          FILE_ATTRIBUTE_NORMAL, FALSE,
                                          nullptr, stream.put());
      if (FAILED(hr)) return hr;
      hr = stream->QueryInterface(IID_PPV_ARGS(mediaStream_.put()));
      if (FAILED(hr)) return hr;
      return S_OK;
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP Initialize(IShellItem* item, DWORD) override {
    try {
      if (!item) return E_POINTER;
      if (mediaStream_) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
      }

      ComPtr<IStream> stream;
      HRESULT hr = item->BindToHandler(nullptr, BHID_Stream,
                                       IID_PPV_ARGS(stream.put()));
      if (SUCCEEDED(hr)) return initializeFromStream(stream.get());

      PWSTR fileSystemPath = nullptr;
      hr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
      if (FAILED(hr) || !fileSystemPath || fileSystemPath[0] == L'\0') {
        CoTaskMemFree(fileSystemPath);
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
      }
      hr = Initialize(fileSystemPath, STGM_READ);
      CoTaskMemFree(fileSystemPath);
      return hr;
    } catch (...) {
      return exceptionToHresult();
    }
  }

  STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap,
                            WTS_ALPHATYPE* alphaType) override {
    try {
      if (!bitmap || !alphaType) return E_POINTER;
      *bitmap = nullptr;
      *alphaType = WTSAT_UNKNOWN;
      if (!mediaStream_) return E_UNEXPECTED;

      const HRESULT imageHr =
          loadWicThumbnailBitmapFromStream(mediaStream_.get(), cx, bitmap,
                                           alphaType);
      if (SUCCEEDED(imageHr)) return imageHr;

      const HRESULT embeddedHr =
          loadEmbeddedArtworkBitmap(mediaStream_.get(), cx, bitmap, alphaType);
      if (SUCCEEDED(embeddedHr)) return embeddedHr;

      return loadVideoFrameThumbnailBitmap(mediaStream_.get(), cx, bitmap,
                                           alphaType);
    } catch (...) {
      return exceptionToHresult();
    }
  }

 private:
  ~RadioifyThumbnailProvider() { radioifyExplorerObjectDestroyed(); }

  HRESULT initializeFromStream(IStream* stream) {
    if (!stream) return E_POINTER;
    if (mediaStream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    return stream->QueryInterface(IID_PPV_ARGS(mediaStream_.put()));
  }

  LONG refCount_ = 1;
  ComPtr<IStream> mediaStream_;
};

class RadioifyThumbnailProviderFactory final : public IClassFactory {
 public:
  RadioifyThumbnailProviderFactory() { radioifyExplorerObjectCreated(); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) delete this;
    return count;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                              void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;

    RadioifyThumbnailProvider* provider =
        new (std::nothrow) RadioifyThumbnailProvider();
    if (!provider) return E_OUTOFMEMORY;

    const HRESULT hr = provider->QueryInterface(riid, object);
    provider->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    radioifyExplorerLockServer(lock != FALSE);
    return S_OK;
  }

 private:
  ~RadioifyThumbnailProviderFactory() { radioifyExplorerObjectDestroyed(); }

  LONG refCount_ = 1;
};

}  // namespace

HRESULT createRadioifyThumbnailProviderFactory(REFIID interfaceId,
                                               void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;

  IClassFactory* factory =
      new (std::nothrow) RadioifyThumbnailProviderFactory();
  if (!factory) return E_OUTOFMEMORY;

  const HRESULT hr = factory->QueryInterface(interfaceId, object);
  factory->Release();
  return hr;
}
