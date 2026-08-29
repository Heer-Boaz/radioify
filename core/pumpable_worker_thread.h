#pragma once

#include <functional>
#include <thread>

#include "native_wait_handle.h"

// Owner-thread wrapper for a finite background operation. Windows signals a
// thread handle only after the thread has actually exited, so finish() never
// turns an approximate "result published" flag into a blocking join.
class PumpableWorkerThread {
 public:
  using Task = std::function<void()>;

  PumpableWorkerThread() = default;
  ~PumpableWorkerThread();

  PumpableWorkerThread(const PumpableWorkerThread&) = delete;
  PumpableWorkerThread& operator=(const PumpableWorkerThread&) = delete;

  bool start(Task task);
  bool joinable() const;
  bool ready() const;
  bool finish();
  void join();
  NativeWaitHandle waitHandle() const;

 private:
  std::thread thread_;
  NativeWaitHandle waitHandle_;
};
