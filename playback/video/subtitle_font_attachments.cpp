#include "subtitle_font_attachments.h"

#include <algorithm>
#include <cctype>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace {

std::string toLowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool hasFontAttachmentExtension(const std::string& filename) {
  const size_t dot = filename.find_last_of('.');
  if (dot == std::string::npos) return false;
  const std::string ext = toLowerAscii(filename.substr(dot));
  return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

bool isFontAttachmentMimeType(const std::string& mimeType) {
  const std::string lower = toLowerAscii(mimeType);
  return lower == "application/x-truetype-font" ||
         lower == "application/x-font-ttf" ||
         lower == "application/vnd.ms-opentype" ||
         lower == "font/ttf" || lower == "font/otf" ||
         lower == "font/collection";
}

std::string attachmentFilename(const AVStream* stream, int fallbackIndex) {
  if (!stream) return "attachment-" + std::to_string(fallbackIndex);
  AVDictionaryEntry* filenameTag =
      av_dict_get(stream->metadata, "filename", nullptr, 0);
  if (filenameTag && filenameTag->value && filenameTag->value[0] != '\0') {
    return filenameTag->value;
  }
  return "attachment-" + std::to_string(fallbackIndex);
}

std::string attachmentMimeType(const AVStream* stream) {
  if (!stream) return {};
  AVDictionaryEntry* mimeTag =
      av_dict_get(stream->metadata, "mimetype", nullptr, 0);
  if (!mimeTag || !mimeTag->value) return {};
  return mimeTag->value;
}

}  // namespace

std::shared_ptr<const SubtitleFontAttachmentList> loadEmbeddedSubtitleFontAttachments(
    AVFormatContext* formatContext) {
  if (!formatContext || formatContext->nb_streams == 0) return {};

  auto attachments = std::make_shared<SubtitleFontAttachmentList>();
  attachments->reserve(formatContext->nb_streams);

  for (unsigned i = 0; i < formatContext->nb_streams; ++i) {
    AVStream* stream = formatContext->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT) continue;
    if (!stream->codecpar->extradata || stream->codecpar->extradata_size <= 0) {
      continue;
    }

    const std::string filename = attachmentFilename(stream, static_cast<int>(i));
    const std::string mimeType = attachmentMimeType(stream);
    if (!isFontAttachmentMimeType(mimeType) &&
        !hasFontAttachmentExtension(filename)) {
      continue;
    }

    SubtitleFontAttachment attachment;
    attachment.filename = filename;
    attachment.data.assign(
        stream->codecpar->extradata,
        stream->codecpar->extradata +
            static_cast<size_t>(stream->codecpar->extradata_size));
    attachments->push_back(std::move(attachment));
  }

  if (attachments->empty()) return {};
  return attachments;
}
