#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "native_wait_handle.h"
#include "waitable_signal.h"

// Runs at most one request at a time and retains only the newest queued work
// and completion. Callers own generation numbers and must commit completions on
// their owning thread.
template <typename Request, typename Result>
class LatestRequestWorker {
 public:
  using Generation = std::uint64_t;

  class Cancellation {
   public:
    bool requested() const {
      return stopping_->load(std::memory_order_acquire) ||
             latestGeneration_->load(std::memory_order_acquire) != generation_;
    }

   private:
    friend class LatestRequestWorker;

    Cancellation(const std::atomic<bool>& stopping,
                 const std::atomic<Generation>& latestGeneration,
                 Generation generation)
        : stopping_(&stopping),
          latestGeneration_(&latestGeneration),
          generation_(generation) {}

    const std::atomic<bool>* stopping_;
    const std::atomic<Generation>* latestGeneration_;
    Generation generation_;
  };

  struct Completion {
    Generation generation;
    std::optional<Result> result;
  };

  using Work = std::function<std::optional<Result>(
      Request request, const Cancellation& cancellation)>;

  explicit LatestRequestWorker(Work work)
      : work_(std::move(work)), worker_([this]() { run(); }) {}

  ~LatestRequestWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_.store(true, std::memory_order_release);
      pending_.reset();
      completion_.reset();
      ready_.clear();
    }
    pendingChanged_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  LatestRequestWorker(const LatestRequestWorker&) = delete;
  LatestRequestWorker& operator=(const LatestRequestWorker&) = delete;

  bool submit(Generation generation, Request request) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_.load(std::memory_order_relaxed)) {
        return false;
      }
      latestGeneration_.store(generation, std::memory_order_release);
      pending_ = Job{generation, std::move(request)};
      completion_.reset();
      ready_.clear();
    }
    pendingChanged_.notify_one();
    return true;
  }

  void cancel(Generation generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    latestGeneration_.store(generation, std::memory_order_release);
    pending_.reset();
    completion_.reset();
    ready_.clear();
  }

  std::optional<Completion> poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completion_) {
      ready_.clear();
      return std::nullopt;
    }
    std::optional<Completion> completion = std::move(completion_);
    completion_.reset();
    ready_.clear();
    return completion;
  }

  NativeWaitHandle nativeWaitHandle() const {
    return ready_.nativeWaitHandle();
  }

 private:
  struct Job {
    Generation generation;
    Request request;
  };

  void run() {
    for (;;) {
      std::optional<Job> nextJob = takeNextJob();
      if (!nextJob) {
        return;
      }
      Job job = std::move(*nextJob);

      const Cancellation cancellation(stopping_, latestGeneration_,
                                      job.generation);
      std::optional<Result> result;
      try {
        result = work_(std::move(job.request), cancellation);
      } catch (...) {
        result.reset();
      }
      if (cancellation.requested()) {
        continue;
      }

      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_.load(std::memory_order_relaxed) ||
          latestGeneration_.load(std::memory_order_relaxed) !=
              job.generation) {
        continue;
      }
      completion_ = Completion{job.generation, std::move(result)};
      ready_.signal();
    }
  }

  std::optional<Job> takeNextJob() {
    std::unique_lock<std::mutex> lock(mutex_);
    pendingChanged_.wait(lock, [this]() {
      return stopping_.load(std::memory_order_relaxed) || pending_.has_value();
    });
    if (stopping_.load(std::memory_order_relaxed)) {
      return std::nullopt;
    }
    Job job = std::move(*pending_);
    pending_.reset();
    return std::optional<Job>(std::move(job));
  }

  Work work_;
  mutable std::mutex mutex_;
  std::condition_variable pendingChanged_;
  std::optional<Job> pending_;
  std::optional<Completion> completion_;
  WaitableSignal ready_;
  std::atomic<bool> stopping_{false};
  std::atomic<Generation> latestGeneration_{0};
  std::thread worker_;
};
