#include "pumpable_worker_thread.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cassert>
#include <utility>

PumpableWorkerThread::~PumpableWorkerThread() { join(); }

bool PumpableWorkerThread::start(Task task) {
  if (!task || thread_.joinable()) return false;
  try {
    thread_ = std::thread(std::move(task));
  } catch (...) {
    return false;
  }
  waitHandle_ = NativeWaitHandle(thread_.native_handle());
  return true;
}

bool PumpableWorkerThread::joinable() const { return thread_.joinable(); }

bool PumpableWorkerThread::ready() const {
  if (!thread_.joinable()) return true;
  const DWORD result =
      WaitForSingleObject(static_cast<HANDLE>(waitHandle_.get()), 0);
  assert(result != WAIT_FAILED);
  return result == WAIT_OBJECT_0;
}

bool PumpableWorkerThread::finish() {
  if (!ready()) return false;
  join();
  return true;
}

void PumpableWorkerThread::join() {
  if (thread_.joinable()) thread_.join();
  waitHandle_ = NativeWaitHandle{};
}

NativeWaitHandle PumpableWorkerThread::waitHandle() const {
  return thread_.joinable() ? waitHandle_ : NativeWaitHandle{};
}
