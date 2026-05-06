#pragma once

class WindowDisplayLifecycle {
 public:
  enum class WorkKind {
    None,
    ClientResize,
    DisplayChange,
  };

  struct Work {
    WorkKind kind = WorkKind::None;
    int width = 0;
    int height = 0;
  };

  class OwnerTransition {
   public:
    explicit OwnerTransition(WindowDisplayLifecycle& lifecycle);
    ~OwnerTransition();

    OwnerTransition(const OwnerTransition&) = delete;
    OwnerTransition& operator=(const OwnerTransition&) = delete;
    OwnerTransition(OwnerTransition&& other) noexcept;
    OwnerTransition& operator=(OwnerTransition&& other) noexcept;

   private:
    WindowDisplayLifecycle* lifecycle_;
  };

  OwnerTransition ownerTransition();
  void clientResized(int width, int height);
  void displayChanged(int width, int height);
  bool consume(Work& out);
  void clear();

 private:
  void beginOwnerTransition();
  void endOwnerTransition();
  bool acceptsExternalWork() const;

  int ownerTransitionDepth_ = 0;
  Work pending_;
};
