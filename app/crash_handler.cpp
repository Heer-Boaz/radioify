#include "crash_handler.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

namespace {

LONG WINAPI writeCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
  if (!exceptionPointers) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  SYSTEMTIME now{};
  GetLocalTime(&now);

  wchar_t path[MAX_PATH]{};
  swprintf(path, MAX_PATH,
           L"radioify_crash_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
           static_cast<unsigned>(now.wYear),
           static_cast<unsigned>(now.wMonth),
           static_cast<unsigned>(now.wDay),
           static_cast<unsigned>(now.wHour),
           static_cast<unsigned>(now.wMinute),
           static_cast<unsigned>(now.wSecond),
           static_cast<unsigned long>(GetCurrentProcessId()));

  HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exceptionPointers;
    info.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpNormal, &info, nullptr, nullptr);
    CloseHandle(file);
  }

  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(&writeCrashDump); }

#else

void installCrashHandler() {}

#endif
