#ifndef TERMINAL_INPUT_SEQUENCE_H
#define TERMINAL_INPUT_SEQUENCE_H

struct InputEvent;

class TerminalInputSequenceParser {
 public:
  enum class Result {
    None,
    Pending,
    Event,
    Rejected,
  };

  Result feed(wchar_t ch, InputEvent& out);
  bool flushPendingEscape(InputEvent& out);
  void reset();

 private:
  bool parse(InputEvent& out, bool* complete);
  bool parseMouse(InputEvent& out, bool* complete) const;
  bool parseKey(InputEvent& out, bool* complete) const;

  wchar_t buffer_[64]{};
  unsigned length_ = 0;
};

#endif
