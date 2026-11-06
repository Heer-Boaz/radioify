#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>

#include "radio.h"
#include "radio_settings_document.h"
#include "radio_settings_schema.h"

namespace {
using namespace radio_settings;

std::string lower(std::string text) {
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::optional<Value> parseValue(const Field& field, const std::string& text) {
  const auto token = trim(text);
  if (token.empty())
    return std::nullopt;
  char* end = nullptr;
  errno = 0;
  switch (field.type) {
    case ValueType::Number: {
      const float value = std::strtof(token.c_str(), &end);
      if (end == token.c_str() || *end != '\0' || errno == ERANGE ||
          !std::isfinite(value))
        return std::nullopt;
      return Value{value};
    }
    case ValueType::Integer: {
      const long value = std::strtol(token.c_str(), &end, 10);
      if (end == token.c_str() || *end != '\0' || errno == ERANGE ||
          value < std::numeric_limits<int>::min() ||
          value > std::numeric_limits<int>::max())
        return std::nullopt;
      return Value{static_cast<int>(value)};
    }
    case ValueType::Boolean: {
      constexpr std::array<std::pair<std::string_view, bool>, 10> tokens{
          {{"true", true},
           {"yes", true},
           {"on", true},
           {"enabled", true},
           {"1", true},
           {"false", false},
           {"no", false},
           {"off", false},
           {"disabled", false},
           {"0", false}}};
      const auto normalized = lower(token);
      for (const auto& entry : tokens)
        if (entry.first == normalized)
          return Value{entry.second};
      return std::nullopt;
    }
  }
  return std::nullopt;
}

const std::vector<RawValue>* selectPreset(const Document& document,
                                          const std::string& name, std::string* error) {
  const auto requested = normalizedName(name.empty() ? "default" : name);
  for (const auto& preset : document.presets) {
    if (normalizedName(preset.first) == requested)
      return &preset.second;
  }
  if (name.empty() && document.presets.size() == 1)
    return &document.presets.begin()->second;
  if (error) {
    if (document.presets.empty()) {
      *error = "audio-filters has no presets.";
    } else {
      std::string available;
      for (const auto& preset : document.presets) {
        if (!available.empty())
          available += ", ";
        available += preset.first;
      }
      *error =
          name.empty()
              ? "Multiple audio-filter presets; supply --radio-preset from: " + available
              : "Unknown audio-filter preset '" + name + "'. Available: " + available;
    }
  }
  return nullptr;
}

struct Change {
  const Field* field;
  Value value;
};

bool resolveChanges(const Document& document, const std::string& preset,
                    std::vector<Change>& changes, std::string* error) {
  // The schema is the only authority for sections, fields, types and targets.
  for (const auto& header : document.headers) {
    if (!findSection(header.name)) {
      if (error)
        *error =
            atLine(header.line, "unknown audio-filter section '" + header.name + "'");
      return false;
    }
  }
  const auto* selected = selectPreset(document, preset, error);
  if (!selected)
    return false;
  changes.reserve(selected->size());
  for (const auto& raw : *selected) {
    const auto* section = findSection(raw.section);
    const auto* field = findField(*section, raw.key);
    const auto path = raw.section + "." + raw.key;
    if (!field) {
      if (error)
        *error = atLine(raw.line, "unknown setting '" + path + "'");
      return false;
    }
    auto value = parseValue(*field, raw.text);
    if (!value) {
      if (error)
        *error = atLine(raw.line, "invalid value for '" + path + "'");
      return false;
    }
    changes.push_back({field, *value});
  }
  return true;
}
}  // namespace

bool applyRadioSettingsIni(Radio1938& radio, const std::string& path,
                           const std::string& presetName, std::string* error) {
  if (error)
    error->clear();
  if (path.size() < 5 || lower(path.substr(path.size() - 5)) != ".toml") {
    if (error)
      *error = "Radio settings file must be a .toml file: " + path;
    return false;
  }
  std::ifstream input(path);
  if (!input) {
    if (error)
      *error = "Failed to open radio settings file: " + path;
    return false;
  }
  Document document;
  if (!parseDocument(input, document, error))
    return false;
  std::vector<Change> changes;
  if (!resolveChanges(document, presetName, changes, error))
    return false;

  // Settings loading is a cold-path transaction. Validate a private candidate
  // against the complete existing configuration, including cross-field rules.
  // Never mutate or reset the live receiver on a parse/validation failure.
  auto candidate = std::make_unique<Radio1938>(radio);
  for (const auto& change : changes)
    assign(*change.field, *candidate, change.value);
  if (!validate(*candidate, error))
    return false;

  // Commit only settings, never the candidate's copied DSP history. A running
  // receiver is rebuilt once after every setting has passed validation.
  for (const auto& change : changes)
    assign(*change.field, radio, change.value);
  if (radio.initialized)
    radio.init(radio.channels, radio.sampleRate, radio.bwHz, radio.noiseWeight);
  return true;
}
