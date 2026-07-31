#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "native_wait_handle.h"
#include "waitable_signal.h"

class ThreadDispatchQueue {
 public:
  ThreadDispatchQueue() = default;
  ~ThreadDispatchQueue();

  ThreadDispatchQueue(const ThreadDispatchQueue&) = delete;
  ThreadDispatchQueue& operator=(const ThreadDispatchQueue&) = delete;

  void openOnCurrentThread();
  void close();

  // Runs on the thread that opened the queue. Returns false when shutdown
  // cancels the work before it begins.
  bool invoke(std::function<void()> task);
  void processPending();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Item {
    explicit Item(std::function<void()> taskIn) : task(std::move(taskIn)) {}

    std::function<void()> task;
    std::mutex mutex;
    std::condition_variable completed;
    std::exception_ptr exception;
    bool done = false;
    bool executed = false;
  };

  static void finish(const std::shared_ptr<Item>& item, bool executed,
                     std::exception_ptr exception = {});

  mutable std::mutex mutex_;
  std::deque<std::shared_ptr<Item>> pending_;
  WaitableSignal ready_;
  std::thread::id targetThread_;
  bool accepting_ = false;
};
