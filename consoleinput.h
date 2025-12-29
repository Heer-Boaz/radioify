#ifndef CONSOLEINPUT_H
#define CONSOLEINPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
};

struct MouseEvent {
  COORD pos{};
  DWORD buttonState = 0;
  DWORD eventFlags = 0;
  DWORD control = 0;
};

struct InputEvent {
  enum class Type {
    None,
    Key,
    Mouse,
    Resize,
  };

  Type type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  COORD size{};
};

class ConsoleInput {
 public:
  void init();
  void restore();
  bool poll(InputEvent& out);
  bool active() const;

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  bool active_ = false;
};

struct DriveEntry {
  std::string label;
  std::filesystem::path path;
};

std::vector<DriveEntry> listDriveEntries();

#endif
