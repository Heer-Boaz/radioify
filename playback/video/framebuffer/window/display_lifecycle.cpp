#include "display_lifecycle.h"

#include <cassert>
#include <utility>

WindowDisplayLifecycle::OwnerTransition::OwnerTransition(
    WindowDisplayLifecycle& lifecycle)
    : lifecycle_(&lifecycle) {
  lifecycle_->beginOwnerTransition();
}

WindowDisplayLifecycle::OwnerTransition::~OwnerTransition() {
  if (lifecycle_) {
    lifecycle_->endOwnerTransition();
  }
}

WindowDisplayLifecycle::OwnerTransition::OwnerTransition(
    OwnerTransition&& other) noexcept
    : lifecycle_(std::exchange(other.lifecycle_, nullptr)) {}

WindowDisplayLifecycle::OwnerTransition&
WindowDisplayLifecycle::OwnerTransition::operator=(
    OwnerTransition&& other) noexcept {
  if (this != &other) {
    if (lifecycle_) {
      lifecycle_->endOwnerTransition();
    }
    lifecycle_ = std::exchange(other.lifecycle_, nullptr);
  }
  return *this;
}

WindowDisplayLifecycle::OwnerTransition
WindowDisplayLifecycle::ownerTransition() {
  return OwnerTransition(*this);
}

void WindowDisplayLifecycle::clientResized(int width, int height) {
  if (!acceptsExternalWork() || width <= 0 || height <= 0) {
    return;
  }
  pending_.width = width;
  pending_.height = height;
  if (pending_.kind == WorkKind::None) {
    pending_.kind = WorkKind::ClientResize;
  }
}

void WindowDisplayLifecycle::displayChanged(int width, int height) {
  if (!acceptsExternalWork() || width <= 0 || height <= 0) {
    return;
  }
  pending_.kind = WorkKind::DisplayChange;
  pending_.width = width;
  pending_.height = height;
}

bool WindowDisplayLifecycle::consume(Work& out) {
  if (pending_.kind == WorkKind::None) {
    return false;
  }
  out = pending_;
  clear();
  return true;
}

void WindowDisplayLifecycle::clear() {
  pending_ = Work{};
}

void WindowDisplayLifecycle::beginOwnerTransition() {
  ++ownerTransitionDepth_;
  clear();
}

void WindowDisplayLifecycle::endOwnerTransition() {
  assert(ownerTransitionDepth_ > 0);
  --ownerTransitionDepth_;
}

bool WindowDisplayLifecycle::acceptsExternalWork() const {
  return ownerTransitionDepth_ == 0;
}
