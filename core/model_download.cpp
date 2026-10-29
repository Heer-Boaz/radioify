#include "core/model_download.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "core/sha256.h"

namespace model_download {
namespace {

struct InternetHandle {
  HINTERNET value = nullptr;
  ~InternetHandle() {
    if (value)
      WinHttpCloseHandle(value);
  }
};

struct ModelFileLock {
  HANDLE value = nullptr;
  bool owned = false;

  ~ModelFileLock() {
    if (owned)
      ReleaseMutex(value);
    if (value)
      CloseHandle(value);
  }
};

bool cancelled(const Control &control) {
  try {
    return control.cancelled && control.cancelled();
  } catch (...) {
    return true;
  }
}

std::filesystem::path receiptPath(const std::filesystem::path &path) {
  std::filesystem::path receipt = path;
  receipt += L".verified";
  return receipt;
}

bool fileIdentity(const std::filesystem::path &path, std::uintmax_t *size,
                  std::int64_t *modifiedTicks) {
  if (!size || !modifiedTicks)
    return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return false;
  *size = std::filesystem::file_size(path, error);
  if (error)
    return false;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error)
    return false;
  *modifiedTicks =
      static_cast<std::int64_t>(modified.time_since_epoch().count());
  return true;
}

bool verifiedReceiptMatches(const std::filesystem::path &path,
                            std::uintmax_t expected, const char *digest) {
  std::uintmax_t size = 0;
  std::int64_t modifiedTicks = 0;
  if (!fileIdentity(path, &size, &modifiedTicks) || size != expected)
    return false;
  std::ifstream input(receiptPath(path), std::ios::binary);
  std::string magic;
  std::string recordedDigest;
  std::uintmax_t recordedSize = 0;
  std::int64_t recordedTicks = 0;
  return input && std::getline(input, magic) &&
         std::getline(input, recordedDigest) &&
         (input >> recordedSize >> recordedTicks) &&
         magic == "radioify-model-verification-v1" &&
         recordedDigest == digest && recordedSize == size &&
         recordedTicks == modifiedTicks;
}

void publishVerifiedReceipt(const std::filesystem::path &path,
                            std::uintmax_t expected, const char *digest) {
  std::uintmax_t size = 0;
  std::int64_t modifiedTicks = 0;
  if (!fileIdentity(path, &size, &modifiedTicks) || size != expected)
    return;
  std::filesystem::path staging = receiptPath(path);
  staging += L".writing-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetCurrentThreadId());
  std::ofstream output(staging, std::ios::binary | std::ios::trunc);
  if (!output)
    return;
  output << "radioify-model-verification-v1\n"
         << digest << '\n'
         << size << '\n'
         << modifiedTicks << '\n';
  output.flush();
  output.close();
  if (!output) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return;
  }
  if (!MoveFileExW(staging.c_str(), receiptPath(path).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
  }
}

bool acquireModelFileLock(const char *digest, const Control &control,
                          ModelFileLock *lock, std::string *error) {
  if (!digest || !lock)
    return false;
  const std::string digestText(digest);
  // Older installations use this name for the same cache. Keep sharing their
  // lock while an application update and an existing download overlap.
  const std::wstring name = L"Local\\Radioify.AnalysisModel." +
                            std::wstring(digestText.begin(), digestText.end());
  lock->value = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!lock->value) {
    if (error)
      *error = "Could not create the model download lock.";
    return false;
  }
  for (;;) {
    const DWORD wait = WaitForSingleObject(lock->value, 100);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      lock->owned = true;
      return true;
    }
    if (wait == WAIT_FAILED) {
      if (error)
        *error = "Could not wait for the model download lock.";
      return false;
    }
    if (cancelled(control)) {
      if (error)
        *error = "Model installation cancelled while another Radioify "
                 "process was updating the same model.";
      return false;
    }
  }
}

bool exactFile(const std::filesystem::path &path, std::uintmax_t expected,
               const char *digest, const Control &control, std::string *error) {
  if (error)
    error->clear();
  if (path.empty()) {
    if (error)
      *error = "no resource path was resolved";
    return false;
  }
  std::error_code fileError;
  const bool regular = std::filesystem::is_regular_file(path, fileError);
  if (fileError) {
    if (error)
      *error =
          "the resource could not be inspected (" + fileError.message() + ")";
    return false;
  }
  if (!regular) {
    if (error)
      *error = "the resource is missing";
    return false;
  }
  const std::uintmax_t actualFileSize =
      std::filesystem::file_size(path, fileError);
  if (fileError) {
    if (error)
      *error =
          "the resource size could not be read (" + fileError.message() + ")";
    return false;
  }
  if (actualFileSize != expected) {
    if (error)
      *error = "the resource size does not match the release manifest";
    return false;
  }
  if (verifiedReceiptMatches(path, expected, digest))
    return true;
  std::uintmax_t actualSize = 0;
  std::string actualDigest;
  if (!core_sha256::file(path, control.cancelled, &actualSize, &actualDigest,
                         error)) {
    return false;
  }
  const bool valid = actualSize == expected && actualDigest == digest;
  if (valid) {
    publishVerifiedReceipt(path, expected, digest);
  } else if (error) {
    *error = "the resource checksum does not match the release manifest";
  }
  return valid;
}

bool queryStatus(HINTERNET request, DWORD *status) {
  if (!request || !status)
    return false;
  DWORD size = sizeof(*status);
  return WinHttpQueryHeaders(
             request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
             WINHTTP_HEADER_NAME_BY_INDEX, status, &size,
             WINHTTP_NO_HEADER_INDEX) != FALSE;
}

Result downloadOne(const std::filesystem::path &destination,
                   const wchar_t *host, const wchar_t *remotePath,
                   const char *progressLabel, std::uintmax_t expectedBytes,
                   const char *expectedDigest, std::uintmax_t completedBefore,
                   const Control &control, bool allowFreshRetry,
                   std::uintmax_t totalDownloadBytes) {
  Result result;
  std::error_code filesystemError;
  std::filesystem::create_directories(destination.parent_path(),
                                      filesystemError);
  if (filesystemError) {
    result.detail = "Could not create the model directory.";
    return result;
  }
  ModelFileLock downloadLock;
  if (!acquireModelFileLock(expectedDigest, control, &downloadLock,
                            &result.detail)) {
    result.status = cancelled(control) ? Status::Cancelled : Status::Failed;
    return result;
  }
  std::string existingError;
  if (exactFile(destination, expectedBytes, expectedDigest, control,
                &existingError)) {
    result.status = Status::Succeeded;
    return result;
  }
  if (cancelled(control)) {
    result.status = Status::Cancelled;
    return result;
  }
  std::filesystem::path staging = destination;
  staging += L".partial";
  std::uintmax_t offset = 0;
  if (std::filesystem::is_regular_file(staging, filesystemError) &&
      !filesystemError) {
    offset = std::filesystem::file_size(staging, filesystemError);
    if (filesystemError || offset > expectedBytes) {
      offset = 0;
      filesystemError.clear();
      std::filesystem::remove(staging, filesystemError);
    }
  }

  if (offset == expectedBytes) {
    if (control.progress) {
      control.progress(static_cast<double>(completedBefore + expectedBytes) /
                           static_cast<double>(totalDownloadBytes),
                       "Verifying resumed model download");
    }
    std::uintmax_t actualSize = 0;
    std::string actualDigest;
    std::string digestError;
    if (!core_sha256::file(staging, control.cancelled, &actualSize,
                           &actualDigest, &digestError)) {
      result.status = cancelled(control) ? Status::Cancelled : Status::Failed;
      result.detail = digestError;
      return result;
    }
    if (actualSize == expectedBytes && actualDigest == expectedDigest) {
      if (!MoveFileExW(staging.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.detail = "Could not publish the verified model.";
        return result;
      }
      publishVerifiedReceipt(destination, expectedBytes, expectedDigest);
      result.status = Status::Succeeded;
      return result;
    }
    filesystemError.clear();
    std::filesystem::remove(staging, filesystemError);
    if (filesystemError) {
      result.detail = "Could not replace an invalid partial model download.";
      return result;
    }
    offset = 0;
  }

  if (control.progress)
    control.progress(static_cast<double>(offset) /
                         static_cast<double>(expectedBytes),
                     progressLabel);
  InternetHandle session;
  session.value =
      WinHttpOpen(L"Radioify model/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session.value) {
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
  }
  InternetHandle connection;
  if (session.value) {
    connection.value =
        WinHttpConnect(session.value, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  }
  InternetHandle request;
  if (connection.value) {
    request.value = WinHttpOpenRequest(
        connection.value, L"GET", remotePath, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  }
  if (!request.value) {
    result.detail = "Could not open the secure model download.";
    return result;
  }
  if (offset > 0) {
    const std::wstring range =
        L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
    WinHttpAddRequestHeaders(
        request.value, range.c_str(), static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  }
  if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    result.detail = "The model download could not be started.";
    return result;
  }
  DWORD status = 0;
  if (!queryStatus(request.value, &status) ||
      (status != 200 && status != 206)) {
    result.detail =
        "The model server returned HTTP " + std::to_string(status) + ".";
    return result;
  }
  if (offset > 0 && status != 206)
    offset = 0;
  const bool resumedDownload = offset > 0;
  std::ofstream output(staging,
                       std::ios::binary |
                           (offset > 0 ? std::ios::app : std::ios::trunc));
  if (!output) {
    result.detail = "Could not create the model staging file.";
    return result;
  }

  std::vector<unsigned char> buffer(1024 * 1024);
  std::uintmax_t received = offset;
  for (;;) {
    if (cancelled(control)) {
      result.status = Status::Cancelled;
      result.detail = "Model installation cancelled; the partial download "
                      "will be resumed next time.";
      return result;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.value, &available)) {
      result.detail = "The model download was interrupted.";
      return result;
    }
    if (available == 0)
      break;
    while (available > 0) {
      const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
          buffer.size(), static_cast<std::size_t>(available)));
      DWORD read = 0;
      if (!WinHttpReadData(request.value, buffer.data(), requested, &read) ||
          read == 0) {
        result.detail = "The model download stopped unexpectedly.";
        return result;
      }
      output.write(reinterpret_cast<const char *>(buffer.data()), read);
      if (!output) {
        result.detail = "Could not write the model staging file.";
        return result;
      }
      received += read;
      available -= std::min(available, read);
      if (control.progress) {
        const double progress =
            static_cast<double>(completedBefore +
                                std::min(received, expectedBytes)) /
            static_cast<double>(totalDownloadBytes);
        control.progress(progress, progressLabel);
      }
      if (received > expectedBytes) {
        result.detail = "The model download exceeded its fixed size.";
        return result;
      }
    }
  }
  output.flush();
  if (!output || received != expectedBytes) {
    result.detail = "The model download is incomplete.";
    return result;
  }
  output.close();

  if (control.progress) {
    control.progress(static_cast<double>(completedBefore + expectedBytes) /
                         static_cast<double>(totalDownloadBytes),
                     "Verifying downloaded model");
  }
  std::uintmax_t actualSize = 0;
  std::string actualDigest;
  std::string digestError;
  if (!core_sha256::file(staging, control.cancelled, &actualSize, &actualDigest,
                         &digestError)) {
    result.status = cancelled(control) ? Status::Cancelled : Status::Failed;
    result.detail = digestError;
    return result;
  }
  if (actualSize != expectedBytes || actualDigest != expectedDigest) {
    filesystemError.clear();
    std::filesystem::remove(staging, filesystemError);
    if (resumedDownload && allowFreshRetry && !filesystemError) {
      if (control.progress) {
        control.progress(static_cast<double>(completedBefore) /
                             static_cast<double>(totalDownloadBytes),
                         "Restarting an invalid resumed model download");
      }
      return downloadOne(destination, host, remotePath, progressLabel,
                         expectedBytes, expectedDigest, completedBefore,
                         control, false, totalDownloadBytes);
    }
    result.detail = "The downloaded model failed SHA-256 verification.";
    return result;
  }
  if (!MoveFileExW(staging.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    result.detail = "Could not publish the verified model.";
    return result;
  }
  publishVerifiedReceipt(destination, expectedBytes, expectedDigest);
  result.status = Status::Succeeded;
  return result;
}

} // namespace

bool verified(const std::filesystem::path &path, const Artifact &artifact,
              const Control &control, std::string *error) {
  return exactFile(path, artifact.bytes, artifact.sha256, control, error);
}

Result ensure(const std::filesystem::path &destination,
              const Artifact &artifact, const Control &control) {
  if (cancelled(control))
    return {Status::Cancelled, "Model download cancelled."};
  return downloadOne(destination, artifact.host, artifact.objectPath,
                     artifact.label, artifact.bytes, artifact.sha256, 0,
                     control, true, artifact.bytes);
}

} // namespace model_download
