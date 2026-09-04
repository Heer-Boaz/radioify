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
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/chapter/integrity.h"

namespace playback_video_chapters {
namespace {

constexpr const wchar_t *kVisionRevision =
    L"48fe6436abf57b3df6ec34f73cdc1fb4b740acb0";
constexpr const wchar_t *kHost = L"huggingface.co";
constexpr const wchar_t *kVisionRepository =
    L"/openbmb/MiniCPM-V-2_6-gguf/resolve/";
constexpr const wchar_t *kModelFile = L"ggml-model-Q4_K_M.gguf";
constexpr const wchar_t *kProjectorFile = L"mmproj-model-f16.gguf";
constexpr const wchar_t *kPlannerRevision =
    L"bf5b95e96dac0462e2a09145ec66cae9a3f12067";
constexpr const wchar_t *kPlannerRepository =
    L"/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/";
constexpr const wchar_t *kPlannerModelFile =
    L"Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf";
constexpr const wchar_t *kSpeechPlanAdapterFile =
    L"chapter-llama-asr-10k-f16.gguf";
constexpr const wchar_t *kChapterPlanAdapterFile =
    L"chapter-llama-captions-asr-10k-f16.gguf";

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

bool cancelled(const OperationControl &control) {
  try {
    return control.cancelled && control.cancelled();
  } catch (...) {
    return true;
  }
}

bool gpuRevoked(const OperationControl &control) {
  try {
    return control.backgroundGpuAllowed && !control.backgroundGpuAllowed();
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

bool nonEmptyRegularFile(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return false;
  return std::filesystem::file_size(path, error) > 0 && !error;
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

bool acquireModelFileLock(const char *digest, const OperationControl &control,
                          ModelFileLock *lock, std::string *error) {
  if (!digest || !lock)
    return false;
  const std::string digestText(digest);
  const std::wstring name = L"Local\\Radioify.ChapterModel." +
                            std::wstring(digestText.begin(), digestText.end());
  lock->value = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!lock->value) {
    if (error)
      *error = "Could not create the chapter-model download lock.";
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
        *error = "Could not wait for the chapter-model download lock.";
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
               const char *digest, const OperationControl &control,
               std::string *error) {
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
  if (!sha256File(path, control.cancelled, &actualSize, &actualDigest, error)) {
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

std::wstring objectPath(const wchar_t *repository, const wchar_t *revision,
                        const wchar_t *file) {
  return std::wstring(repository) + revision + L"/" + file + L"?download=true";
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

InstallResult
downloadOne(const std::filesystem::path &destination, const wchar_t *repository,
            const wchar_t *revision, const wchar_t *remoteFile,
            const char *progressLabel, std::uintmax_t expectedBytes,
            const char *expectedDigest, std::uintmax_t completedBefore,
            const OperationControl &control, bool allowFreshRetry = true) {
  InstallResult result;
  std::error_code filesystemError;
  std::filesystem::create_directories(destination.parent_path(),
                                      filesystemError);
  if (filesystemError) {
    result.detail = "Could not create the chapter-model directory.";
    return result;
  }
  ModelFileLock downloadLock;
  if (!acquireModelFileLock(expectedDigest, control, &downloadLock,
                            &result.detail)) {
    result.status = cancelled(control) ? OperationStatus::Cancelled
                                       : OperationStatus::Failed;
    return result;
  }
  std::string existingError;
  if (exactFile(destination, expectedBytes, expectedDigest, control,
                &existingError)) {
    result.status = OperationStatus::Succeeded;
    return result;
  }
  if (cancelled(control)) {
    result.status = OperationStatus::Cancelled;
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
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.detail = "Could not publish the verified chapter model.";
        return result;
      }
      publishVerifiedReceipt(destination, expectedBytes, expectedDigest);
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
      L"Radioify chapter model/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session.value) {
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
  }
  InternetHandle connection;
  if (session.value) {
    connection.value =
        WinHttpConnect(session.value, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  }
  InternetHandle request;
  const std::wstring path = objectPath(repository, revision, remoteFile);
  if (connection.value) {
    request.value = WinHttpOpenRequest(
        connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
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
  if (offset > 0 && status != 206)
    offset = 0;
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
    if (available == 0)
      break;
    while (available > 0) {
      const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
          buffer.size(), static_cast<std::size_t>(available)));
      DWORD read = 0;
      if (!WinHttpReadData(request.value, buffer.data(), requested, &read) ||
          read == 0) {
        result.detail = "The chapter-model download stopped unexpectedly.";
        return result;
      }
      output.write(reinterpret_cast<const char *>(buffer.data()), read);
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
        control.progress(progress, progressLabel);
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
    control.progress(static_cast<double>(completedBefore + expectedBytes) /
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
        control.progress(static_cast<double>(completedBefore) /
                             static_cast<double>(kModelDownloadBytes),
                         "Restarting an invalid resumed model download");
      }
      return downloadOne(destination, repository, revision, remoteFile,
                         progressLabel, expectedBytes, expectedDigest,
                         completedBefore, control, false);
    }
    result.detail = "The downloaded chapter model failed SHA-256 verification.";
    return result;
  }
  if (!MoveFileExW(staging.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    result.detail = "Could not publish the verified chapter model.";
    return result;
  }
  publishVerifiedReceipt(destination, expectedBytes, expectedDigest);
  result.status = OperationStatus::Succeeded;
  return result;
}

} // namespace

ModelPaths resolveModelPaths() {
  ModelPaths paths;
  paths.directory =
      radioifyWritableDataDir() / "models" / "minicpm-v-2-6-q4-k-m";
  paths.model = paths.directory / kModelFile;
  paths.projector = paths.directory / kProjectorFile;
  paths.plannerDirectory =
      radioifyWritableDataDir() / "models" / "chapter-llama-8b-q4-k-m";
  paths.plannerModel = paths.plannerDirectory / kPlannerModelFile;
  const std::vector<std::filesystem::path> resourceRoots =
      radioifyResourceSearchRoots();
  const std::filesystem::path relativeSpeechPlanAdapter =
      std::filesystem::path("models") / "chapter_analysis" /
      kSpeechPlanAdapterFile;
  const std::filesystem::path relativeChapterPlanAdapter =
      std::filesystem::path("models") / "chapter_analysis" /
      kChapterPlanAdapterFile;
  if (!resourceRoots.empty())
    paths.speechPlanAdapter =
        resourceRoots.front() / relativeSpeechPlanAdapter;
  if (!resourceRoots.empty())
    paths.chapterPlanAdapter =
        resourceRoots.front() / relativeChapterPlanAdapter;
  for (const std::filesystem::path &root : resourceRoots) {
    const std::filesystem::path candidate = root / relativeSpeechPlanAdapter;
    std::error_code fileError;
    if (std::filesystem::is_regular_file(candidate, fileError) && !fileError) {
      paths.speechPlanAdapter = candidate;
    }
    const std::filesystem::path chapterCandidate =
        root / relativeChapterPlanAdapter;
    fileError.clear();
    if (std::filesystem::is_regular_file(chapterCandidate, fileError) &&
        !fileError)
      paths.chapterPlanAdapter = chapterCandidate;
  }
  return paths;
}

CapabilityResult inspectPackagedChapterRuntime() {
  const std::filesystem::path executableRoot = radioifyExecutableDir();
  if (executableRoot.empty()) {
    return {CapabilityState::Unsupported,
            "The executable directory could not be resolved."};
  }

  const std::filesystem::path chapterRoot =
      executableRoot / "models" / "chapter_analysis";
  const std::filesystem::path speechPlanAdapter =
      chapterRoot / kSpeechPlanAdapterFile;
  const std::filesystem::path chapterPlanAdapter =
      chapterRoot / kChapterPlanAdapterFile;
  std::string verificationError;
  if (!exactFile(speechPlanAdapter, kSpeechPlanAdapterBytes,
                 kSpeechPlanAdapterSha256, {}, &verificationError)) {
    std::string detail =
        "The staged Chapter-Llama ASR plan adapter could not be verified";
    if (!verificationError.empty())
      detail += ": " + verificationError;
    detail += ".";
    return {CapabilityState::Unsupported, std::move(detail)};
  }
  if (!exactFile(chapterPlanAdapter, kChapterPlanAdapterBytes,
                 kChapterPlanAdapterSha256, {}, &verificationError)) {
    std::string detail =
        "The staged Chapter-Llama captions-plus-ASR adapter could not be verified";
    if (!verificationError.empty())
      detail += ": " + verificationError;
    detail += ".";
    return {CapabilityState::Unsupported, std::move(detail)};
  }

  const std::filesystem::path requiredFiles[] = {
      executableRoot / "radioify_chapter_worker.exe",
      executableRoot / "llama-cpp-LICENSE.txt",
      chapterRoot / "CHAPTER-LLAMA-NOTICE.md",
      chapterRoot / "LLAMA-3.1-LICENSE",
      chapterRoot / "NOTICE",
  };
  for (const std::filesystem::path &required : requiredFiles) {
    if (!nonEmptyRegularFile(required)) {
      return {CapabilityState::Unsupported,
              "The staged chapter runtime is missing " +
                  toUtf8String(required.filename()) + "."};
    }
  }
  return {CapabilityState::Ready, {}};
}

CapabilityResult inspectModelArtifacts(const ModelPaths &paths,
                                       const OperationControl &control) {
  if (control.progress) {
    control.progress(std::nullopt, "Verifying MiniCPM-V 2.6 model");
  }
  std::string error;
  const bool modelReady =
      exactFile(paths.model, kModelBytes, kModelSha256, control, &error);
  if (cancelled(control)) {
    return {CapabilityState::Cancelled, {}};
  }
  if (gpuRevoked(control))
    return {CapabilityState::Yielded, {}};
  const bool projectorReady = exactFile(paths.projector, kProjectorBytes,
                                        kProjectorSha256, control, &error);
  if (cancelled(control)) {
    return {CapabilityState::Cancelled, {}};
  }
  if (gpuRevoked(control))
    return {CapabilityState::Yielded, {}};
  const bool plannerReady = exactFile(paths.plannerModel, kPlannerModelBytes,
                                      kPlannerModelSha256, control, &error);
  if (cancelled(control)) {
    return {CapabilityState::Cancelled, {}};
  }
  if (gpuRevoked(control))
    return {CapabilityState::Yielded, {}};
  const bool speechPlanAdapterReady =
      exactFile(paths.speechPlanAdapter, kSpeechPlanAdapterBytes,
                kSpeechPlanAdapterSha256, control, &error);
  if (cancelled(control))
    return {CapabilityState::Cancelled, {}};
  if (gpuRevoked(control))
    return {CapabilityState::Yielded, {}};
  const bool chapterPlanAdapterReady =
      exactFile(paths.chapterPlanAdapter, kChapterPlanAdapterBytes,
                kChapterPlanAdapterSha256, control, &error);
  if (cancelled(control))
    return {CapabilityState::Cancelled, {}};
  if (gpuRevoked(control))
    return {CapabilityState::Yielded, {}};
  if (!speechPlanAdapterReady) {
    std::string detail = "The packaged Chapter-Llama ASR plan adapter could "
                         "not be verified";
    if (!error.empty())
      detail += ": " + error;
    detail += ".";
    return {CapabilityState::Unsupported, std::move(detail)};
  }
  if (!chapterPlanAdapterReady) {
    std::string detail = "The packaged Chapter-Llama captions-plus-ASR adapter could "
                         "not be verified";
    if (!error.empty())
      detail += ": " + error;
    detail += ".";
    return {CapabilityState::Unsupported, std::move(detail)};
  }
  if (!modelReady || !projectorReady || !plannerReady) {
    return {CapabilityState::SetupRequired,
            "Install the verified visual and chapter-planning models to "
            "enable automatic video chapters."};
  }
  return {CapabilityState::Ready, {}};
}

InstallResult installModelArtifacts(const ModelPaths &paths,
                                    const OperationControl &control) {
  std::string verificationError;
  const bool speechPlanAdapterReady =
      exactFile(paths.speechPlanAdapter, kSpeechPlanAdapterBytes,
                kSpeechPlanAdapterSha256, control, &verificationError);
  if (cancelled(control))
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  if (gpuRevoked(control))
    return {OperationStatus::Yielded, "Model installation yielded."};
  if (!speechPlanAdapterReady) {
    std::string detail = "The packaged Chapter-Llama ASR plan adapter could "
                         "not be verified";
    if (!verificationError.empty())
      detail += ": " + verificationError;
    detail += ".";
    return {OperationStatus::Unsupported, std::move(detail)};
  }
  const bool chapterPlanAdapterReady =
      exactFile(paths.chapterPlanAdapter, kChapterPlanAdapterBytes,
                kChapterPlanAdapterSha256, control, &verificationError);
  if (cancelled(control))
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  if (!chapterPlanAdapterReady) {
    std::string detail =
        "The packaged Chapter-Llama captions-plus-ASR adapter could not be verified";
    if (!verificationError.empty())
      detail += ": " + verificationError;
    detail += ".";
    return {OperationStatus::Unsupported, std::move(detail)};
  }
  bool modelReady = exactFile(paths.model, kModelBytes, kModelSha256, control,
                              &verificationError);
  if (cancelled(control)) {
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  }
  if (!modelReady) {
    InstallResult model =
        downloadOne(paths.model, kVisionRepository, kVisionRevision, kModelFile,
                    "Downloading MiniCPM-V 2.6 visual model", kModelBytes,
                    kModelSha256, 0, control);
    if (model.status != OperationStatus::Succeeded)
      return model;
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
        downloadOne(paths.projector, kVisionRepository, kVisionRevision,
                    kProjectorFile, "Downloading MiniCPM-V 2.6 projector",
                    kProjectorBytes, kProjectorSha256, kModelBytes, control);
    if (projector.status != OperationStatus::Succeeded)
      return projector;
  }
  bool plannerReady =
      exactFile(paths.plannerModel, kPlannerModelBytes, kPlannerModelSha256,
                control, &verificationError);
  if (cancelled(control)) {
    return {OperationStatus::Cancelled, "Model installation cancelled."};
  }
  if (!plannerReady) {
    InstallResult planner =
        downloadOne(paths.plannerModel, kPlannerRepository, kPlannerRevision,
                    kPlannerModelFile, "Downloading Chapter-Llama base model",
                    kPlannerModelBytes, kPlannerModelSha256,
                    kModelBytes + kProjectorBytes, control);
    if (planner.status != OperationStatus::Succeeded)
      return planner;
  }
  if (control.progress)
    control.progress(1.0, "Model installation complete");
  return {OperationStatus::Succeeded, {}};
}

} // namespace playback_video_chapters
