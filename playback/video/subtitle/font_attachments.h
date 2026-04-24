#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVFormatContext;

struct SubtitleFontAttachment {
  std::string filename;
  std::vector<uint8_t> data;
};

using SubtitleFontAttachmentList = std::vector<SubtitleFontAttachment>;

std::shared_ptr<const SubtitleFontAttachmentList> loadEmbeddedSubtitleFontAttachments(
    AVFormatContext* formatContext);
