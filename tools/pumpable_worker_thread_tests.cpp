#include "core/pumpable_worker_thread.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "pumpable_worker_thread_tests: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  PumpableWorkerThread worker;
  std::mutex mutex;
  std::condition_variable changed;
  bool resultPublished = false;
  bool mayExit = false;

  ok &= expect(worker.start([&]() {
    std::unique_lock<std::mutex> lock(mutex);
    resultPublished = true;
    changed.notify_all();
    changed.wait(lock, [&]() { return mayExit; });
  }),
               "a new task must start");

  {
    std::unique_lock<std::mutex> lock(mutex);
    ok &= expect(changed.wait_for(lock, std::chrono::seconds(2),
                                  [&]() { return resultPublished; }),
                 "the controlled worker must publish its result");
  }

  const NativeWaitHandle runningHandle = worker.waitHandle();
  ok &= expect(worker.joinable() && runningHandle && !worker.ready() &&
                   !worker.finish() &&
                   WaitForSingleObject(static_cast<HANDLE>(runningHandle.get()),
                                       0) == WAIT_TIMEOUT,
               "published output must not make a running worker joinable on "
               "the owner thread");

  {
    std::lock_guard<std::mutex> lock(mutex);
    mayExit = true;
  }
  changed.notify_all();
  ok &= expect(WaitForSingleObject(static_cast<HANDLE>(runningHandle.get()),
                                   2000) == WAIT_OBJECT_0,
               "the kernel handle must signal when the worker exits");
  ok &= expect(worker.ready() && worker.finish() && !worker.joinable() &&
                   !worker.waitHandle(),
               "finish must reclaim only an exited worker");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
