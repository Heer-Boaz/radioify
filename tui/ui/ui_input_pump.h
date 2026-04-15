#pragma once

#include <optional>

#include "consoleinput.h"

class ConsoleInputPump {
 public:
  bool pollNext(ConsoleInput& input, InputEvent& out);

 private:
  std::optional<InputEvent> queued_;
};
