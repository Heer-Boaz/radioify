#pragma once

#include <mutex>

namespace windows_file_drop {

// Serializes the cross-thread modal policy with the OLE drag lifecycle. The
// window thread owns enter/over/drop, while the application owner may disable
// acceptance as soon as a modal surface appears.
class AcceptanceState {
 public:
  struct Transition {
    bool accept = false;
    bool beginHover = false;
    bool cancelHover = false;
  };

  bool setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (enabled_ || !hovering_) return false;
    hovering_ = false;
    return true;
  }

  bool enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
  }

  Transition update(bool sourceCanDrop) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool accept = enabled_ && sourceCanDrop;
    const bool wasHovering = hovering_;
    hovering_ = accept;
    return Transition{accept, accept && !wasHovering,
                      !accept && wasHovering};
  }

  Transition complete(bool sourceCanDrop) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool accept = enabled_ && sourceCanDrop;
    const bool wasHovering = hovering_;
    hovering_ = false;
    return Transition{accept, false, wasHovering && !accept};
  }

  bool leave() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool wasHovering = hovering_;
    hovering_ = false;
    return wasHovering;
  }

 private:
  mutable std::mutex mutex_;
  bool enabled_ = true;
  bool hovering_ = false;
};

}  // namespace windows_file_drop
