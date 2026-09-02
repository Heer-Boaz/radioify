#include "playback/video/chapter/model.h"

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
#include <system_error>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/chapter/integrity.h"

namespace playback_video_chapters {
namespace {

constexpr const wchar_t* kPinnedRevision =
    L"508edd0afaa66bb9e9f40587acc2184f02daf1f6";
constexpr const wchar_t* kHost = L"huggingface.co";
constexpr const wchar_t* kRepository =
    L"/ggml-org/Qwen2.5-VL-7B-Instruct-GGUF/resolve/";
constexpr const wchar_t* kModelFile = L"Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf";
constexpr const wchar_t* kProjectorFile =
    L"mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf";

struct InternetHandle {
  HINTERNET value = nullptr;
  ~InternetHandle() {
    if (value) WinHttpCloseHandle(value);
  }
};

bool cancelled(const OperationControl& control) {
  try {
    return control.cancelled && control.cancelled();
  } catch (...) {
    return true;
  }
}

bool gpuRevoked(const OperationControl& control) {
  try {
    return control.backgroundGpuAllowed &&
           !control.backgroundGpuAllowed();
  } catch (...) {
    return true;
  }
}

bool exactFile(const std::filesystem::path& path, std::uintmax_t expected,
               const char* digest, const OperationControl& control,
               std::string* error) {
  std::error_code fileError;
  if (!std::filesystem::is_regular_file(path, fileError) || fileError ||
      std::filesystem::file_size(path, fileError) != expected || fileError) {
    return false;
  }
  std::uintmax_t actualSize = 0;
  std::string actualDigest;
  const auto interrupted = [&]() {
    return cancelled(control) || gpuRevoked(control);
  };
  if (!sha256File(path, interrupted, &actualSize, &actualDigest, error)) {
    return false;
  }
  return actualSize == expected && actualDigest == digest;
}

std::wstring objectPath(const wchar_t* file) {
  return std::wstring(kRepository) + kPinnedRevision + L"/" + file +
         L"?download=true";
}

bool queryStatus(HINTERNET request, DWORD* status) {
  if (!request || !status) return false;
  DWORD size = sizeof(*status);
  return WinHttpQueryHeaders(
             request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
             WINHTTP_HEADER_NAME_BY_INDEX, status, &size,
             WINHTTP_NO_HEADER_INDEX) != FALSE;
}

InstallResult downloadOne(const std::filesystem::path& destination,
                          const wchar_t* remoteFile,
                          std::uintmax_t expectedBytes,
                          const char* expectedDigest,
                          std::uintmax_t completedBefore,
                          const OperationControl& control,
                          bool allowFreshRetry = true) {
  InstallResult result;
  std::error_code filesystemError;
  std::filesystem::create_directories(destination.parent_path(),
                                      filesystemError);
  if (filesystemError) {
    result.detail = "Could not create the chapter-model directory.";
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
      control.progress(
          static_cast<double>(completedBefore + expectedBytes) /
              static_cast<double>(kModelDownloadBytes),
          "Verifying resumed model download");
    }
    std::uintmax_t actualSize = 0;
    std::string actualDigest;
    std::string digestError;
    if (!sha256File(staging, control.cancelled, &actualSize, &actualDigest,
                    &digestError)) {
      result.status = cancelled(control) ? OperationStatus::Cancelled
                                         : OperationStatus::Failed;
      result.detail = digestError;
      return result;
    }
    if (actualSize == expectedBytes && actualDigest == expectedDigest) {
      if (!MoveFileExW(staging.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH)) {
        result.detail = "Could not publish the verified chapter model.";
        return result;
      }
      result.status = OperationStatus::Succeeded;
      return result;
    }
    filesystemError.clear();
    std::filesystem::remove(staging, filesystemError);
    if (filesystemError) {
      result.detail =
          "Could not replace an invalid partial chapter-model download.";
      return result;
    }
    offset = 0;
  }

  InternetHandle session;
  session.value = WinHttpOpen(
      L"Radioify chapter model/1.0",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (session.value) {
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
  }
  InternetHandle connection;
  if (session.value) {
    connection.value = WinHttpConnect(session.value, kHost,
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
  }
  InternetHandle request;
  const std::wstring path = objectPath(remoteFile);
  if (connection.value) {
    request.value = WinHttpOpenRequest(
        connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
  }
  if (!request.value) {
    result.detail = "Could not open the secure model download.";
    return result;
  }
  if (offset > 0) {
    const std::wstring range =
        L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
    WinHttpAddRequestHeaders(request.value, range.c_str(),
                             static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD |
                                 WINHTTP_ADDREQ_FLAG_REPLACE);
  }
  if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    result.detail = "The chapter-model download could not be started.";
    return result;
  }
  DWORD status = 0;
  if (!queryStatus(request.value, &status) ||
      (status != 200 && status != 206)) {
    result.detail = "The chapter-model server returned HTTP " +
                    std::to_string(status) + ".";
    return result;
  }
  if (offset > 0 && status != 206) offset = 0;
  const bool resumedDownload = offset > 0;
  std::ofstream output(staging,
                       std::ios::binary |
                           (offset > 0 ? std::ios::app : std::ios::trunc));
  if (!output) {
    result.detail = "Could not create the chapter-model staging file.";
    return result;
  }

  std::vector<unsigned char> buffer(1024 * 1024);
  std::uintmax_t received = offset;
  for (;;) {
    if (cancelled(control)) {
      result.status = OperationStatus::Cancelled;
      result.detail = "Model installation cancelled; the partial download "
                      "will be resumed next time.";
      return result;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.value, &available)) {
      result.detail = "The chapter-model download was interrupted.";
      return result;
    }
    if (available == 0) break;
    while (available > 0) {
      const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
          buffer.size(), static_cast<std::size_t>(available)));
      DWORD read = 0;
      if (!WinHttpReadData(request.value, buffer.data(), requested, &read) ||
          read == 0) {
        result.detail = "The chapter-model download stopped unexpectedly.";
        return result;
      }
      output.write(reinterpret_cast<const char*>(buffer.data()), read);
      if (!output) {
        result.detail = "Could not write the chapter-model staging file.";
        return result;
      }
      received += read;
      available -= std::min(available, read);
      if (control.progress) {
        const double progress =
            static_cast<double>(completedBefore +
                                std::min(received, expectedBytes)) /
            static_cast<double>(kModelDownloadBytes);
        control.progress(progress, "Downloading Qwen2.5-VL 7B");
      }
      if (received > expectedBytes) {
        result.detail = "The chapter-model download exceeded its fixed size.";
        return result;
      }
    }
  }
  output.flush();
  if (!output || received != expectedBytes) {
    result.detail = "The chapter-model download is incomplete.";
    return result;
  }
  output.close();

  if (control.progress) {
    control.progress(
        static_cast<double>(completedBefore + expectedBytes) /
            static_cast<double>(kModelDownloadBytes),
        "Verifying downloaded model");
  }
  std::uintmax_t actualSize = 0;
  std::string actualDigest;
  std::string digestError;
  if (!sha256File(staging, control.cancelled, &actualSize, &actualDigest,
                  &digestError)) {
    result.status = cancelled(control) ? OperationStatus::Cancelled
                                       : OperationStatus::Failed;
    result.detail = digestError;
    return result;
  }
  if (actualSize != expectedBytes || actualDigest != expectedDigest) {
    filesystemError.clear();
    std::filesystem::remove(staging, filesystemError);
    if (resumedDownload && allowFreshRetry && !filesystemError) {
      if (control.progress) {
        control.progress(
            static_cast<double>(completedBefore) /
                static_cast<double>(kModelDownloadBytes),
            "Restarting an invalid resumed model download");
      }
      return downloadOne(destination, remoteFile, expectedBytes,
                         expectedDigest, completedBefore, control, false);
    }
    result.detail =
        "The downloaded chapter model failed SHA-256 verification.";
    return result;
  }
  if (!MoveFileExW(staging.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    result.detail = "Could not publish the verified chapter model.";
    return result;
  }
  result.status = OperationStatus::Succeeded;
  return result;
}

}  // namespace

ModelPaths resolveModelPaths() {
  ModelPaths paths;
  paths.directory =
      radioifyWritableDataDir() / "models" / "qwen2.5-vl-7b-instruct-q4-k-m";
  paths.model = paths.directory / kModelFile;
  paths.projector = paths.directory / kProjectorFile;
  return paths;
}

CapabilityResult inspectModelArtifacts(const ModelPaths& paths,
                                       const OperationControl& control) {
  if (control.progress) {
    control.progress(std::nullopt, "Verifying Qwen2.5-VL model");
  }
  std::string error;
  const bool modelReady =
      exactFile(paths.model, kModelBytes, kModelSha256, control, &error);
  if (cancelled(control)) {
    return {CapabilityState::Cancelled, {}};
  }
  if (gpuRevoked(control)) return {CapabilityState::Yielded, {}};
  const bool projectorReady = exactFile(paths.projector, kProjectorBytes,
                                        kProjectorSha256, control, &error);
  if (cancelled(control)) {
    return {CapabilityState::Cancelled, {}};
  }
  if (gpuRevoked(control)) return {CapabilityState::Yielded, {}};
  if (!modelReady || !projectorReady) {
    return {CapabilityState::SetupRequired,
            "Install the verified 5.54 GB Qwen2.5-VL model to enable automatic "
            "video chapters."};
  }
  return {CapabilityState::Ready, {}};
}

InstallResult installModelArtifacts(const ModelPaths& paths,
                                    const OperationControl& control) {
  std::string verificationError;
  bool modelReady = exactFile(paths.model, kModelBytes, kModelSha256,
                              control, &verificationError);
  if (cancelled(control)) {
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  }
  if (!modelReady) {
    InstallResult model = downloadOne(paths.model, kModelFile, kModelBytes,
                                      kModelSha256, 0, control);
    if (model.status != OperationStatus::Succeeded) return model;
  } else if (control.progress) {
    control.progress(static_cast<double>(kModelBytes) /
                         static_cast<double>(kModelDownloadBytes),
                     "Main model already verified");
  }

  bool projectorReady =
      exactFile(paths.projector, kProjectorBytes, kProjectorSha256, control,
                &verificationError);
  if (cancelled(control)) {
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  }
  if (!projectorReady) {
    InstallResult projector =
        downloadOne(paths.projector, kProjectorFile, kProjectorBytes,
                    kProjectorSha256, kModelBytes, control);
    if (projector.status != OperationStatus::Succeeded) return projector;
  }
  if (control.progress) control.progress(1.0, "Model installation complete");
  return {OperationStatus::Succeeded, {}};
}

}  // namespace playback_video_chapters
