#pragma once

#include <string_view>

void activateWindowsConsoleWindow();
bool isRadioifyConsoleForegroundWindow();
bool radioifyConsoleWindowTitleMatches(std::wstring_view title);
