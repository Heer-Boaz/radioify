#include "shell_open_mode.h"

#include <algorithm>
#include <string>

namespace {

std::string trimAscii(std::string_view value) {
  size_t first = 0;
  while (first < value.size() &&
         static_cast<unsigned char>(value[first]) <= ' ') {
    ++first;
  }
  size_t last = value.size();
  while (last > first &&
         static_cast<unsigned char>(value[last - 1]) <= ' ') {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch >= 'A' && ch <= 'Z') {
                     return static_cast<char>(ch - 'A' + 'a');
                   }
                   return static_cast<char>(ch);
                 });
  return value;
}

}  // namespace

bool parseShellOpenMode(std::string_view value, ShellOpenMode& out) {
  const std::string mode = toLowerAscii(trimAscii(value));
  if (mode == "same-instance" || mode == "same" ||
      mode == "single-instance" || mode == "single") {
    out = ShellOpenMode::SameInstance;
    return true;
  }
  if (mode == "new-instance" || mode == "new") {
    out = ShellOpenMode::NewInstance;
    return true;
  }
  return false;
}

bool parseShellOpenModeSelection(std::string_view value,
                                 ShellOpenModeSelection& out) {
  ShellOpenMode mode = ShellOpenMode::SameInstance;
  if (!parseShellOpenMode(value, mode)) {
    return false;
  }
  out = mode == ShellOpenMode::SameInstance
            ? ShellOpenModeSelection::SameInstance
            : ShellOpenModeSelection::NewInstance;
  return true;
}

ShellOpenMode resolveShellOpenModeSelection(
    ShellOpenModeSelection selection,
    ShellOpenMode configuredMode) {
  switch (selection) {
    case ShellOpenModeSelection::SameInstance:
      return ShellOpenMode::SameInstance;
    case ShellOpenModeSelection::NewInstance:
      return ShellOpenMode::NewInstance;
    case ShellOpenModeSelection::Configured:
      return configuredMode;
  }
  return configuredMode;
}
