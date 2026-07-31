#include "thread_dispatch_queue.h"

#include <cassert>
#include <utility>

ThreadDispatchQueue::~ThreadDispatchQueue() { close(); }

void ThreadDispatchQueue::openOnCurrentThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(!accepting_ && pending_.empty());
  targetThread_ = std::this_thread::get_id();
  accepting_ = true;
  ready_.clear();
}

void ThreadDispatchQueue::close() {
  std::deque<std::shared_ptr<Item>> cancelled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    targetThread_ = {};
    cancelled.swap(pending_);
    ready_.clear();
  }
  for (const auto& item : cancelled) {
    finish(item, false);
  }
}

bool ThreadDispatchQueue::invoke(std::function<void()> task) {
  assert(task);

  std::shared_ptr<Item> item;
  bool executeInline = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
      return false;
    }
    executeInline = targetThread_ == std::this_thread::get_id();
    if (!executeInline) {
      item = std::make_shared<Item>(std::move(task));
      pending_.push_back(item);
      ready_.signal();
    }
  }

  if (executeInline) {
    task();
    return true;
  }

  std::unique_lock<std::mutex> lock(item->mutex);
  item->completed.wait(lock, [&]() { return item->done; });
  if (item->exception) {
    std::rethrow_exception(item->exception);
  }
  return item->executed;
}

void ThreadDispatchQueue::processPending() {
  for (;;) {
    std::shared_ptr<Item> item;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!accepting_) {
        return;
      }
      assert(targetThread_ == std::this_thread::get_id());
      if (pending_.empty()) {
        ready_.clear();
        return;
      }
      item = std::move(pending_.front());
      pending_.pop_front();
      if (pending_.empty()) {
        ready_.clear();
      }
    }

    std::exception_ptr exception;
    try {
      item->task();
    } catch (...) {
      exception = std::current_exception();
    }
    finish(item, true, std::move(exception));
  }
}

NativeWaitHandle ThreadDispatchQueue::nativeWaitHandle() const {
  return ready_.nativeWaitHandle();
}

void ThreadDispatchQueue::finish(const std::shared_ptr<Item>& item,
                                 bool executed,
                                 std::exception_ptr exception) {
  {
    std::lock_guard<std::mutex> lock(item->mutex);
    item->executed = executed;
    item->exception = std::move(exception);
    item->done = true;
  }
  item->completed.notify_one();
}
