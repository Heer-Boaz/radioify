#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "core/thread_dispatch_queue.h"
#include "core/waitable_signal.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "thread_dispatch_queue_tests: %s\n", message);
    return false;
  }
  return true;
}

void waitFor(const WaitableSignal& signal) {
  WaitForSingleObject(static_cast<HANDLE>(signal.nativeWaitHandle().get()),
                      INFINITE);
}

}  // namespace

int main() {
  bool ok = true;
  ThreadDispatchQueue queue;
  bool ran = false;

  ok &= expect(!queue.invoke([&]() { ran = true; }),
               "a closed queue must reject work");
  ok &= expect(!ran, "rejected work must not execute");

  WaitableSignal opened;
  std::thread worker([&]() {
    queue.openOnCurrentThread();
    opened.signal();
    WaitForSingleObject(
        static_cast<HANDLE>(queue.nativeWaitHandle().get()), INFINITE);
    queue.processPending();
    queue.close();
  });
  waitFor(opened);

  const std::thread::id callerThread = std::this_thread::get_id();
  std::thread::id executionThread;
  ok &= expect(queue.invoke([&]() {
                 executionThread = std::this_thread::get_id();
               }),
               "queued work must complete synchronously");
  worker.join();
  ok &= expect(executionThread != callerThread,
               "queued work must execute on the target thread");

  queue.openOnCurrentThread();
  executionThread = {};
  ok &= expect(queue.invoke([&]() {
                 executionThread = std::this_thread::get_id();
               }),
               "target-thread invocation must execute directly");
  ok &= expect(executionThread == std::this_thread::get_id(),
               "target-thread invocation must not enqueue itself");

  std::atomic<bool> cancelledWorkRan{false};
  std::atomic<bool> cancelledInvocationReturned{false};
  std::thread blockedCaller([&]() {
    const bool executed = queue.invoke([&]() {
      cancelledWorkRan.store(true, std::memory_order_relaxed);
    });
    cancelledInvocationReturned.store(!executed, std::memory_order_relaxed);
  });
  WaitForSingleObject(static_cast<HANDLE>(queue.nativeWaitHandle().get()),
                      INFINITE);
  queue.close();
  blockedCaller.join();
  ok &= expect(cancelledInvocationReturned.load(std::memory_order_relaxed),
               "closing must release a synchronous caller");
  ok &= expect(!cancelledWorkRan.load(std::memory_order_relaxed),
               "closing must not execute pending work on the wrong thread");

  return ok ? 0 : 1;
}
