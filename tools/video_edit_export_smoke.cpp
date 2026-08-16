#include "playback/video/edit/export.h"

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/edit/export_metadata.h"

namespace {

bool parseRange(const std::wstring& text,
                playback_video_edit::SourceRange* range) {
  if (!range) return false;
  const size_t separator = text.find(L':');
  if (separator == std::wstring::npos) return false;
  try {
    range->startUs = std::stoll(text.substr(0, separator));
    range->endUs = std::stoll(text.substr(separator + 1));
  } catch (...) {
    return false;
  }
  return range->startUs >= 0 && range->endUs > range->startUs;
}

int runAuditSelfTest() {
  using playback_video_edit::detail::CadenceFingerprint;
  using playback_video_edit::detail::equalFrameCadence;

  CadenceFingerprint source;
  for (const int64_t pts : {0, 37, 90, 127, 180}) {
    source.observe(pts, AVRational{1, 1000});
  }
  CadenceFingerprint equivalent;
  for (const int64_t pts : {0, 37'000, 90'000, 127'000, 180'000}) {
    equivalent.observe(pts, AVRational{1, 1'000'000});
  }
  std::string error;
  if (!equalFrameCadence(source, equivalent, &error)) {
    std::cerr << "equivalent cadence clocks compared unequal: " << error
              << '\n';
    return 1;
  }

  CadenceFingerprint quantized;
  for (const int64_t pts : {0, 1, 2, 3, 4}) {
    quantized.observe(pts, AVRational{4, 89});
  }
  error.clear();
  if (equalFrameCadence(source, quantized, &error) || error.empty()) {
    std::cerr << "VFR-to-CFR quantization escaped the cadence audit\n";
    return 1;
  }
  std::cout << "video_edit_export_cadence_audit: PASS\n";
  return 0;
}

struct StreamSignature {
  AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
  AVCodecID codec = AV_CODEC_ID_NONE;
  int profile = AV_PROFILE_UNKNOWN;
  int disposition = 0;
  int width = 0;
  int height = 0;
  int pixelDepth = 0;
  int chromaWidthShift = 0;
  int chromaHeightShift = 0;
  uint64_t pixelFlags = 0;
  AVFieldOrder fieldOrder = AV_FIELD_UNKNOWN;
  AVRational sampleAspect{0, 1};
  AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
  AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
  AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
  AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
  int sampleRate = 0;
  int channels = 0;
  std::string channelLayout;
  int rawBits = 0;
  int codedBits = 0;
  std::string language;
  std::string title;
};

struct MediaSignature {
  std::string formatNames;
  int64_t durationUs = 0;
  std::vector<StreamSignature> streams;
};

std::string metadataValue(const AVDictionary* metadata, const char* key) {
  const AVDictionaryEntry* entry = av_dict_get(metadata, key, nullptr, 0);
  return entry && entry->value ? entry->value : "";
}

std::string channelLayoutName(const AVCodecParameters* parameters) {
  if (!parameters || parameters->ch_layout.nb_channels <= 0) return {};
  AVChannelLayout layout{};
  if (parameters->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC &&
      av_channel_layout_check(&parameters->ch_layout)) {
    if (av_channel_layout_copy(&layout, &parameters->ch_layout) < 0) return {};
  } else {
    av_channel_layout_default(&layout, parameters->ch_layout.nb_channels);
  }
  char name[128]{};
  av_channel_layout_describe(&layout, name, sizeof(name));
  av_channel_layout_uninit(&layout);
  return name;
}

bool inspectMedia(const std::filesystem::path& path, MediaSignature* signature,
                  std::string* error) {
  if (!signature) return false;
  AVFormatContext* format = nullptr;
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(format, nullptr);
  if (result < 0 || !format) {
    avformat_close_input(&format);
    if (error) *error = "media could not be probed";
    return false;
  }
  signature->formatNames = format->iformat && format->iformat->name
                               ? format->iformat->name
                               : "";
  signature->durationUs = format->duration;
  signature->streams.clear();
  signature->streams.reserve(format->nb_streams);
  for (unsigned index = 0; index < format->nb_streams; ++index) {
    AVStream* stream = format->streams[index];
    const AVCodecParameters* parameters = stream->codecpar;
    StreamSignature current;
    current.type = parameters->codec_type;
    current.codec = parameters->codec_id;
    current.profile = parameters->profile;
    current.disposition = stream->disposition;
    current.language = metadataValue(stream->metadata, "language");
    current.title = metadataValue(stream->metadata, "title");
    current.rawBits = parameters->bits_per_raw_sample;
    current.codedBits = parameters->bits_per_coded_sample;
    if (current.type == AVMEDIA_TYPE_VIDEO) {
      current.width = parameters->width;
      current.height = parameters->height;
      current.sampleAspect = parameters->sample_aspect_ratio;
      current.fieldOrder = parameters->field_order;
      current.colorRange = parameters->color_range;
      current.colorPrimaries = parameters->color_primaries;
      current.colorTransfer = parameters->color_trc;
      current.colorSpace = parameters->color_space;
      current.chromaLocation = parameters->chroma_location;
      const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(
          static_cast<AVPixelFormat>(parameters->format));
      if (pixel) {
        current.pixelDepth = pixel->comp[0].depth;
        current.chromaWidthShift = pixel->log2_chroma_w;
        current.chromaHeightShift = pixel->log2_chroma_h;
        current.pixelFlags =
            pixel->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA |
                            AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_PAL |
                            AV_PIX_FMT_FLAG_BAYER);
      }
    } else if (current.type == AVMEDIA_TYPE_AUDIO) {
      current.sampleRate = parameters->sample_rate;
      current.channels = parameters->ch_layout.nb_channels;
      current.channelLayout = channelLayoutName(parameters);
    }
    signature->streams.push_back(std::move(current));
  }
  avformat_close_input(&format);
  return true;
}

bool formatFamiliesIntersect(const std::string& left,
                             const std::string& right) {
  size_t leftStart = 0;
  while (leftStart <= left.size()) {
    const size_t leftEnd = left.find(',', leftStart);
    const std::string leftName = left.substr(
        leftStart, leftEnd == std::string::npos ? std::string::npos
                                                : leftEnd - leftStart);
    size_t rightStart = 0;
    while (rightStart <= right.size()) {
      const size_t rightEnd = right.find(',', rightStart);
      if (right.compare(rightStart,
                        rightEnd == std::string::npos
                            ? right.size() - rightStart
                            : rightEnd - rightStart,
                        leftName) == 0 &&
          (rightEnd == std::string::npos
               ? right.size() - rightStart
               : rightEnd - rightStart) == leftName.size()) {
        return true;
      }
      if (rightEnd == std::string::npos) break;
      rightStart = rightEnd + 1;
    }
    if (leftEnd == std::string::npos) break;
    leftStart = leftEnd + 1;
  }
  return false;
}

bool sameAspect(AVRational left, AVRational right) {
  if (left.num <= 0 || left.den <= 0) left = AVRational{1, 1};
  if (right.num <= 0 || right.den <= 0) right = AVRational{1, 1};
  return av_cmp_q(left, right) == 0;
}

bool validateOutput(const MediaSignature& source,
                    const MediaSignature& output,
                    int64_t expectedDurationUs, std::string* error) {
  if (!formatFamiliesIntersect(source.formatNames, output.formatNames)) {
    if (error) *error = "container family changed";
    return false;
  }
  if (source.streams.size() != output.streams.size()) {
    if (error) *error = "stream count changed";
    return false;
  }
  bool video = false;
  for (size_t index = 0; index < source.streams.size(); ++index) {
    const StreamSignature& before = source.streams[index];
    const StreamSignature& after = output.streams[index];
    if (before.type != after.type || before.codec != after.codec ||
        (before.profile != AV_PROFILE_UNKNOWN &&
         before.profile != after.profile) ||
        before.disposition != after.disposition ||
        before.language != after.language || before.title != after.title) {
      if (error) *error = "stream identity or metadata changed at index " +
                          std::to_string(index);
      return false;
    }
    if (before.type == AVMEDIA_TYPE_VIDEO) {
      video = true;
      if (before.width != after.width || before.height != after.height ||
          before.pixelDepth != after.pixelDepth ||
          before.chromaWidthShift != after.chromaWidthShift ||
          before.chromaHeightShift != after.chromaHeightShift ||
          before.pixelFlags != after.pixelFlags ||
          (before.fieldOrder != AV_FIELD_UNKNOWN &&
           before.fieldOrder != after.fieldOrder) ||
          !sameAspect(before.sampleAspect, after.sampleAspect) ||
          before.colorRange != after.colorRange ||
          before.colorPrimaries != after.colorPrimaries ||
          before.colorTransfer != after.colorTransfer ||
          before.colorSpace != after.colorSpace ||
          before.chromaLocation != after.chromaLocation) {
        if (error) {
          *error = "video geometry, depth, or color changed";
        }
        return false;
      }
    } else if (before.type == AVMEDIA_TYPE_AUDIO) {
      const AVCodecDescriptor* description =
          avcodec_descriptor_get(before.codec);
      const bool lossless =
          description && (description->props & AV_CODEC_PROP_LOSSLESS) != 0;
      if (before.sampleRate != after.sampleRate ||
          before.channels != after.channels ||
          before.channelLayout != after.channelLayout ||
          (lossless && before.rawBits > 0 &&
           before.rawBits != after.rawBits) ||
          (lossless && before.codedBits > 0 &&
           before.codedBits != after.codedBits)) {
        if (error) *error = "audio rate, layout, or bit depth changed";
        return false;
      }
    }
  }
  if (!video || output.durationUs <= 0) {
    if (error) *error = "export has no playable video timeline";
    return false;
  }
  const int64_t durationDifference =
      output.durationUs >= expectedDurationUs
          ? output.durationUs - expectedDurationUs
          : expectedDurationUs - output.durationUs;
  if (durationDifference > 150'000) {
    if (error) {
      *error = "export duration differs from the edit list by " +
               std::to_string(durationDifference) + " us";
    }
    return false;
  }
  std::cout << "probe duration_us=" << output.durationUs
            << " streams=" << output.streams.size() << '\n';
  return true;
}

bool selectInputVideo(const std::filesystem::path& path, int* videoIndex) {
  AVFormatContext* format = nullptr;
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(format, nullptr);
  int video = -1;
  if (result >= 0) {
    video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  }
  avformat_close_input(&format);
  if (video < 0) return false;
  if (videoIndex) *videoIndex = video;
  return true;
}

std::set<std::filesystem::path> temporarySiblings(
    const std::filesystem::path& destination) {
  std::set<std::filesystem::path> paths;
  const std::wstring prefix =
      destination.filename().wstring() + L".radioify-part-";
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(destination.parent_path(), error)) {
    if (error) break;
    const std::wstring filename = entry.path().filename().wstring();
    if (filename.rfind(prefix, 0) == 0) paths.insert(entry.path());
  }
  return paths;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") {
    return runAuditSelfTest();
  }
  if (argc < 4) {
    std::cerr << "usage: video_edit_export_smoke <input> <output> "
                 "[--cancel|--expect-failure|--smooth-cut|"
                 "--expect-smooth-failure|--smooth-first-cut] "
                 "<start_us:end_us> [range...]\n";
    return 2;
  }
  playback_video_edit::ExportRequest request;
  request.sourcePath = argv[1];
  request.destinationPath = argv[2];
  if (!selectInputVideo(request.sourcePath, &request.videoStreamIndex)) {
    std::cerr << "input streams could not be selected\n";
    return 2;
  }
  const std::wstring mode = argv[3];
  const bool cancelTest = mode == L"--cancel";
  const bool expectedSmoothFailure = mode == L"--expect-smooth-failure";
  const bool expectedFailure =
      mode == L"--expect-failure" || expectedSmoothFailure;
  const bool smoothCutTest =
      mode == L"--smooth-cut" || expectedSmoothFailure;
  const bool mixedCutTest = mode == L"--smooth-first-cut";
  const int firstRange =
      cancelTest || expectedFailure || smoothCutTest || mixedCutTest ? 4 : 3;
  if (firstRange >= argc) {
    std::cerr << "at least one range is required\n";
    return 2;
  }
  for (int index = firstRange; index < argc; ++index) {
    playback_video_edit::SourceRange range;
    if (!parseRange(argv[index], &range)) {
      std::cerr << "invalid range\n";
      return 2;
    }
    request.decisions.keptRanges.push_back(range);
  }
  if (!std::is_sorted(request.decisions.keptRanges.begin(),
                      request.decisions.keptRanges.end(),
                      [](const auto& lhs, const auto& rhs) {
                        return lhs.startUs < rhs.startUs;
                      })) {
    std::cerr << "ranges must be source ordered\n";
    return 2;
  }
  int64_t expectedDurationUs = 0;
  request.decisions.cutTransitions.assign(
      request.decisions.keptRanges.size() > 1
          ? request.decisions.keptRanges.size() - 1
          : 0,
      playback_video_edit::CutTransition::hard());
  if (smoothCutTest || mixedCutTest) {
    if (request.decisions.cutTransitions.empty()) {
      std::cerr << "smooth-cut modes require at least two ranges\n";
      return 2;
    }
    if (smoothCutTest) {
      std::fill(request.decisions.cutTransitions.begin(),
                request.decisions.cutTransitions.end(),
                playback_video_edit::CutTransition::motionSmooth());
    } else {
      request.decisions.cutTransitions.front() =
          playback_video_edit::CutTransition::motionSmooth();
    }
  }
  for (const playback_video_edit::SourceRange& range :
       request.decisions.keptRanges) {
    expectedDurationUs += range.durationUs();
  }
  MediaSignature sourceSignature;
  std::string probeError;
  if (!inspectMedia(request.sourcePath, &sourceSignature, &probeError)) {
    std::cerr << "source probe failed: " << probeError << '\n';
    return 2;
  }

  const std::set<std::filesystem::path> temporaryFilesBefore =
      temporarySiblings(request.destinationPath);
  playback_video_edit::Exporter exporter;
  if (!exporter.start(std::move(request))) {
    std::cerr << "export worker did not start\n";
    return 1;
  }
  if (cancelTest) exporter.cancel();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    exporter.consumeChanged();
    const playback_video_edit::ExportSnapshot snapshot = exporter.snapshot();
    if (!snapshot.finished()) continue;
    if (cancelTest) {
      const bool clean =
          snapshot.state == playback_video_edit::ExportState::Cancelled &&
          !std::filesystem::exists(snapshot.destinationPath) &&
          temporarySiblings(snapshot.destinationPath) == temporaryFilesBefore;
      if (!clean) {
        std::cerr << "cancelled export published or leaked a partial file\n";
        return 1;
      }
      std::cout << "cancelled cleanly\n";
      return 0;
    }
    if (expectedFailure) {
      const bool clean =
          snapshot.state == playback_video_edit::ExportState::Failed &&
          !snapshot.error.empty() &&
          !std::filesystem::exists(snapshot.destinationPath) &&
          temporarySiblings(snapshot.destinationPath) == temporaryFilesBefore;
      if (!clean) {
        std::cerr << "failed export published or leaked a partial file\n";
        return 1;
      }
      std::cout << "failed cleanly: " << snapshot.error << '\n';
      return 0;
    }
    if (snapshot.state != playback_video_edit::ExportState::Succeeded) {
      std::cerr << "export failed: " << snapshot.error << '\n';
      return 1;
    }
    std::cout << "encoder=" << snapshot.videoEncoder << '\n';
    MediaSignature outputSignature;
    if (!inspectMedia(snapshot.destinationPath, &outputSignature, &probeError) ||
        !validateOutput(sourceSignature, outputSignature, expectedDurationUs,
                        &probeError)) {
      std::cerr << probeError << '\n';
      return 1;
    }
    return 0;
  }
}
