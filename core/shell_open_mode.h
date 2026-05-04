#pragma once

#include <string_view>

enum class ShellOpenMode {
  SameInstance,
  NewInstance,
};

enum class ShellOpenModeSelection {
  Configured,
  SameInstance,
  NewInstance,
};

bool parseShellOpenMode(std::string_view value, ShellOpenMode& out);
bool parseShellOpenModeSelection(std::string_view value,
                                 ShellOpenModeSelection& out);
ShellOpenMode resolveShellOpenModeSelection(ShellOpenModeSelection selection,
                                            ShellOpenMode configuredMode);
