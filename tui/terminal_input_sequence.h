#ifndef TERMINAL_INPUT_SEQUENCE_H
#define TERMINAL_INPUT_SEQUENCE_H

#include <cstdint>

#include "input_event.h"

class TerminalInputSequenceParser {
 public:
  enum class Result {
    None,
    Pending,
    Event,
    Rejected,
  };

  Result feed(wchar_t ch, InputEvent& out);
  Result feed(wchar_t ch, InputEvent& out, KeyPressKind pressKind,
              std::uint32_t repeatCount);
  bool flushPendingEscape(InputEvent& out);
  void reset();

 private:
  bool parse(InputEvent& out, bool* complete);
  bool parseMouse(InputEvent& out, bool* complete) const;
  bool parseLegacyMouse(InputEvent& out, bool* complete) const;
  bool parseKey(InputEvent& out, bool* complete) const;

  wchar_t buffer_[64]{};
  unsigned length_ = 0;
  KeyPressKind pressKind_ = KeyPressKind::Initial;
  std::uint32_t repeatCount_ = 1;
};

#endif
