#include "ui_input_pump.h"

bool ConsoleInputPump::pollNext(ConsoleInput& input, InputEvent& out) {
  if (queued_.has_value()) {
    out = *queued_;
    queued_.reset();
    return true;
  }

  if (!input.poll(out)) {
    return false;
  }
  if (out.type != InputEvent::Type::Resize) {
    return true;
  }

  InputEvent next{};
  while (input.poll(next)) {
    if (next.type == InputEvent::Type::Resize) {
      out = next;
      continue;
    }
    queued_ = next;
    break;
  }

  return true;
}
