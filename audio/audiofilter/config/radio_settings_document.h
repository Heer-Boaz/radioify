#ifndef RADIOIFY_AUDIOFILTER_CONFIG_RADIO_SETTINGS_DOCUMENT_H
#define RADIOIFY_AUDIOFILTER_CONFIG_RADIO_SETTINGS_DOCUMENT_H

#include <istream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace radio_settings {

struct RawValue {
  int line;
  std::string section;
  std::string key;
  std::string text;
};
struct SectionHeader {
  int line;
  std::string name;
};
struct Document {
  std::map<std::string, std::vector<RawValue>> presets;
  std::vector<SectionHeader> headers;
};

// The established scalar settings format: named audio-filter tables, comments,
// and number/integer/boolean values. No receiver types or section names here.
bool parseDocument(std::istream& input, Document& document, std::string* error);
std::string normalizedName(std::string_view name);
std::string trim(std::string_view text);
std::string atLine(int line, const std::string& message);

}  // namespace radio_settings
#endif
