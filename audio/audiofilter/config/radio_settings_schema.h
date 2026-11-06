#ifndef RADIOIFY_AUDIOFILTER_CONFIG_RADIO_SETTINGS_SCHEMA_H
#define RADIOIFY_AUDIOFILTER_CONFIG_RADIO_SETTINGS_SCHEMA_H

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

#include "../radio1938/radio_pipeline_types.h"

struct Radio1938;

namespace radio_settings {

using Value = std::variant<float, int, bool>;
using Target = std::variant<float& (*)(Radio1938&), int& (*)(Radio1938&),
                            bool& (*)(Radio1938&), PassId>;
enum class ValueType { Number, Integer, Boolean };

struct Field {
  std::string_view name;
  ValueType type;
  Target target;
};

struct Section {
  std::string_view name;
  const Field* fields;
  size_t count;
  bool (*validate)(const Radio1938&, std::string*);
};

struct Schema {
  const Section* sections;
  size_t count;
};

Schema schema();
const Section* findSection(std::string_view name);
const Field* findField(const Section& section, std::string_view name);
Value read(const Field& field, Radio1938& radio);
void assign(const Field& field, Radio1938& radio, const Value& value);
bool validate(const Radio1938& radio, std::string* error);

}  // namespace radio_settings
#endif
