#include "radio_settings_document.h"

#include <cctype>

namespace radio_settings {

std::string normalizedName(std::string_view name) {
  std::string result;
  for (unsigned char c : name)
    if (std::isalnum(c))
      result += static_cast<char>(std::tolower(c));
  return result;
}

std::string trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return std::string(text);
}

std::string atLine(int line, const std::string& message) {
  return "line " + std::to_string(line) + ": " + message;
}

namespace {
std::string withoutComment(std::string_view line) {
  char quote = 0;
  bool escaped = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quote == '"' && c == '\\') {
      escaped = true;
      continue;
    }
    if (quote && c == quote) {
      quote = 0;
      continue;
    }
    if (!quote && (c == '\'' || c == '"')) {
      quote = c;
      continue;
    }
    if (!quote && (c == '#' || c == ';'))
      return trim(line.substr(0, i));
  }
  return trim(line);
}
}  // namespace

bool parseDocument(std::istream& input, Document& document, std::string* error) {
  Document parsed;
  int lineNumber = 0;
  std::string preset, section, line;
  const auto fail = [&](const std::string& message) {
    if (error)
      *error = atLine(lineNumber, message);
    return false;
  };
  while (std::getline(input, line)) {
    ++lineNumber;
    if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
      line.erase(0, 3);
    line = withoutComment(line);
    if (line.empty())
      continue;
    if (line.front() == '[') {
      if (line.back() != ']')
        return fail("unterminated section header");
      const std::string header = trim(std::string_view(line).substr(1, line.size() - 2));
      const auto first = header.find('.');
      const auto second =
          first == std::string::npos ? first : header.find('.', first + 1);
      if (first == std::string::npos || second == std::string::npos ||
          normalizedName(header.substr(0, first)) != "audiofilter") {
        return fail("expected [audio-filter.<preset>.<section>]");
      }
      preset = trim(std::string_view(header).substr(first + 1, second - first - 1));
      section = normalizedName(std::string_view(header).substr(second + 1));
      if (preset.empty() || section.empty())
        return fail("empty preset or section name");
      parsed.headers.push_back({lineNumber, section});
      parsed.presets.try_emplace(preset);
      continue;
    }
    if (preset.empty())
      return fail("setting outside an audio-filter section");
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      return fail("expected key = value");
    const std::string key = normalizedName(std::string_view(line).substr(0, separator));
    if (key.empty())
      return fail("missing key name");
    std::string value = trim(std::string_view(line).substr(separator + 1));
    if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') &&
        value.back() == value.front())
      value = value.substr(1, value.size() - 2);
    parsed.presets[preset].push_back({lineNumber, section, key, std::move(value)});
  }
  if (input.bad())
    return fail("failed while reading settings");
  document = std::move(parsed);
  return true;
}

}  // namespace radio_settings
