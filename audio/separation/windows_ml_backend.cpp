#include "audio/separation/windows_ml_backend.h"
#include "audio/separation/windows_ml_backend_internal.h"

#include <windows.h>
#include <appmodel.h>

#include <WinMLEpCatalog.h>
#include <WinMLAsync.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace audio_separation {
namespace {

constexpr std::string_view kNvidiaProviderName =
    "NvTensorRtRtxExecutionProvider";
constexpr double kWindowsMlProgressMaximum = 100.0;

std::string hresultDescription(HRESULT result) {
  std::ostringstream description;
  description << "0x" << std::uppercase << std::hex << std::setw(8)
              << std::setfill('0')
              << static_cast<std::uint32_t>(result);
  return description.str();
}

bool equalAsciiCaseInsensitive(std::string_view left,
                               std::string_view right) {
  if (left.size() != right.size()) return false;
  return std::equal(left.begin(), left.end(), right.begin(),
                    [](char lhs, char rhs) {
                      return std::tolower(static_cast<unsigned char>(lhs)) ==
                             std::tolower(static_cast<unsigned char>(rhs));
                    });
}

struct CatalogOwner {
  WinMLEpCatalogHandle handle = nullptr;

  ~CatalogOwner() {
    if (handle) WinMLEpCatalogRelease(handle);
  }
  CatalogOwner() = default;
  CatalogOwner(const CatalogOwner&) = delete;
  CatalogOwner& operator=(const CatalogOwner&) = delete;
};

struct MatchingProvider {
  WinMLEpHandle handle = nullptr;
  std::string name;
  std::string version;
  WinMLEpReadyState readyState = WinMLEpReadyState_NotPresent;
  bool uncertifiedCandidateSeen = false;
};

int readinessRank(WinMLEpReadyState state) {
  switch (state) {
    case WinMLEpReadyState_Ready:
      return 3;
    case WinMLEpReadyState_NotReady:
      return 2;
    case WinMLEpReadyState_NotPresent:
      return 1;
  }
  return 0;
}

std::array<std::uint32_t, 4> parseVersion(std::string_view version) {
  std::array<std::uint32_t, 4> parts{};
  std::size_t part = 0;
  while (!version.empty() && part < parts.size()) {
    const std::size_t separator = version.find('.');
    const std::string_view token = version.substr(0, separator);
    std::uint32_t value = 0;
    const auto parsed =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
      return {};
    }
    parts[part++] = value;
    if (separator == std::string_view::npos) break;
    version.remove_prefix(separator + 1);
  }
  return parts;
}

bool isNewerVersion(std::string_view candidate, std::string_view current) {
  const auto candidateParts = parseVersion(candidate);
  const auto currentParts = parseVersion(current);
  if (candidateParts != currentParts) return candidateParts > currentParts;
  return candidate > current;
}

BOOL CALLBACK findNvidiaProvider(WinMLEpHandle provider,
                                 const WinMLEpInfo* info, void* context) {
  if (!provider || !info || !info->name || !context ||
      !equalAsciiCaseInsensitive(info->name, kNvidiaProviderName)) {
    return TRUE;
  }
  auto* match = static_cast<MatchingProvider*>(context);
  if (info->certification != WinMLEpCertification_Certified) {
    match->uncertifiedCandidateSeen = true;
    return TRUE;
  }
  const int candidateRank = readinessRank(info->readyState);
  const int currentRank = readinessRank(match->readyState);
  if (match->handle && candidateRank < currentRank) return TRUE;
  if (match->handle && candidateRank == currentRank && info->version &&
      !match->version.empty() &&
      !isNewerVersion(info->version, match->version)) {
    return TRUE;
  }
  match->handle = provider;
  match->name = info->name;
  match->version = info->version ? info->version : "";
  match->readyState = info->readyState;
  return TRUE;
}

bool readLibraryPath(WinMLEpHandle provider,
                     std::filesystem::path* libraryPath,
                     std::string* detail) {
  size_t required = 0;
  HRESULT result = WinMLEpGetLibraryPathSize(provider, &required);
  if (FAILED(result) || required == 0) {
    if (detail) {
      *detail = "Windows ML could not report the NVIDIA provider library "
                "path (" +
                hresultDescription(result) + ").";
    }
    return false;
  }
  std::string utf8Path(required, '\0');
  size_t used = 0;
  result =
      WinMLEpGetLibraryPath(provider, utf8Path.size(), utf8Path.data(), &used);
  if (FAILED(result)) {
    if (detail) {
      *detail = "Windows ML could not read the NVIDIA provider library path (" +
                hresultDescription(result) + ").";
    }
    return false;
  }
  if (used > 0 && used <= utf8Path.size()) utf8Path.resize(used);
  while (!utf8Path.empty() && utf8Path.back() == '\0') utf8Path.pop_back();
  if (utf8Path.empty()) {
    if (detail) *detail = "Windows ML returned an empty provider library path.";
    return false;
  }
  const std::u8string nativeUtf8(
      reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size());
  *libraryPath = std::filesystem::path(nativeUtf8);
  return true;
}

template <typename SizeReader, typename ValueReader>
std::string readProviderText(WinMLEpHandle provider, SizeReader readSize,
                             ValueReader readValue) {
  size_t required = 0;
  if (FAILED(readSize(provider, &required)) || required == 0) {
    return {};
  }
  std::string value(required, '\0');
  size_t used = 0;
  if (FAILED(readValue(provider, value.size(), value.data(), &used))) {
    return {};
  }
  if (used > 0 && used <= value.size()) value.resize(used);
  while (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
}

std::string packageVersionFromFullName(
    const std::wstring& packageFullName) {
  if (packageFullName.empty()) return {};

  UINT32 bufferLength = 0;
  const LONG sizeResult =
      PackageIdFromFullName(packageFullName.c_str(), 0, &bufferLength,
                            nullptr);
  if (sizeResult != ERROR_INSUFFICIENT_BUFFER || bufferLength == 0) return {};
  std::vector<BYTE> buffer(bufferLength);
  const LONG readResult = PackageIdFromFullName(
      packageFullName.c_str(), 0, &bufferLength, buffer.data());
  if (readResult != ERROR_SUCCESS) return {};
  const auto* identity = reinterpret_cast<const PACKAGE_ID*>(buffer.data());
  std::ostringstream version;
  version << identity->version.Major << '.' << identity->version.Minor << '.'
          << identity->version.Build << '.' << identity->version.Revision;
  return version.str();
}

std::string packageVersionFromRoot(std::string_view utf8Root) {
  if (utf8Root.empty()) return {};
  const std::u8string nativeUtf8(
      reinterpret_cast<const char8_t*>(utf8Root.data()), utf8Root.size());
  return packageVersionFromFullName(
      std::filesystem::path(nativeUtf8).filename().wstring());
}

std::string packageVersionFromInstalledPath(
    std::filesystem::path installedPath) {
  installedPath = installedPath.parent_path();
  while (!installedPath.empty()) {
    std::string version =
        packageVersionFromFullName(installedPath.filename().wstring());
    if (!version.empty()) return version;
    const std::filesystem::path parent = installedPath.parent_path();
    if (parent == installedPath) break;
    installedPath = parent;
  }
  return {};
}

std::string readProviderVersion(WinMLEpHandle provider) {
  std::string version = readProviderText(
      provider, WinMLEpGetVersionSize, WinMLEpGetVersion);
  if (!version.empty()) return version;
  const std::string packageRoot = readProviderText(
      provider, WinMLEpGetPackageRootPathSize, WinMLEpGetPackageRootPath);
  return packageVersionFromRoot(packageRoot);
}

WindowsMlBackendResolution failedResolution(std::string detail) {
  WindowsMlBackendResolution resolution;
  resolution.status = WindowsMlBackendStatus::Failed;
  resolution.detail = std::move(detail);
  return resolution;
}

bool discoverNvidiaProvider(CatalogOwner* catalog, MatchingProvider* match,
                            WindowsMlBackendResolution* failure) {
  if (!catalog || !match || !failure) return false;
  const HRESULT createResult = WinMLEpCatalogCreate(&catalog->handle);
  if (FAILED(createResult) || !catalog->handle) {
    failure->status = WindowsMlBackendStatus::Unavailable;
    failure->detail =
        "The Windows ML execution-provider catalog is unavailable (" +
        hresultDescription(createResult) + ").";
    return false;
  }

  const HRESULT enumerateResult =
      WinMLEpCatalogEnumProviders(catalog->handle, findNvidiaProvider, match);
  if (FAILED(enumerateResult)) {
    *failure = failedResolution(
        "Windows ML could not enumerate execution providers (" +
        hresultDescription(enumerateResult) + ").");
    return false;
  }
  if (!match->handle) {
    failure->status = WindowsMlBackendStatus::Unavailable;
    failure->detail = match->uncertifiedCandidateSeen
                          ? "Windows ML offered only uncertified NVIDIA "
                            "TensorRT-RTX providers."
                          : "Windows ML did not offer a compatible NVIDIA "
                            "TensorRT-RTX provider.";
    return false;
  }
  return true;
}

WindowsMlBackendResolution readyResolution(const MatchingProvider& match) {
  WindowsMlBackendResolution resolution;
  resolution.version = match.version;
  std::filesystem::path libraryPath;
  if (!readLibraryPath(match.handle, &libraryPath, &resolution.detail)) {
    resolution.status = WindowsMlBackendStatus::Failed;
    return resolution;
  }
  if (const std::string activeVersion = readProviderVersion(match.handle);
      !activeVersion.empty()) {
    resolution.version = activeVersion;
  }
  if (resolution.version.empty()) {
    resolution.version = packageVersionFromInstalledPath(libraryPath);
  }
  InferenceBackend backend;
  backend.kind = InferenceBackendKind::WindowsMlNvidiaTensorRtRtx;
  backend.providerName = match.name;
  backend.displayName = "NVIDIA TensorRT-RTX";
  backend.diagnosticComponent = "nvidia-tensorrt-rtx";
  backend.providerVersion = resolution.version;
  backend.providerLibrary = std::move(libraryPath);
  resolution.status = WindowsMlBackendStatus::Ready;
  resolution.backend = std::move(backend);
  resolution.detail = "The certified NVIDIA TensorRT-RTX provider is ready.";
  return resolution;
}

struct AsyncSetupState {
  std::mutex mutex;
  std::condition_variable changed;
  bool completed = false;
  bool progressChanged = false;
  double progress = 0.0;
};

void CALLBACK providerSetupCompleted(WinMLAsyncBlock* async) {
  if (!async || !async->context) return;
  auto* state = static_cast<AsyncSetupState*>(async->context);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->completed = true;
  }
  state->changed.notify_one();
}

void CALLBACK providerSetupProgress(WinMLAsyncBlock* async, double progress) {
  if (!async || !async->context) return;
  auto* state = static_cast<AsyncSetupState*>(async->context);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->progress =
        std::clamp(progress / kWindowsMlProgressMaximum, 0.0, 1.0);
    state->progressChanged = true;
  }
  state->changed.notify_one();
}

WindowsMlProviderSetupResult failedSetup(std::string detail) {
  WindowsMlProviderSetupResult result;
  result.outcome = WindowsMlProviderSetupOutcome::Failed;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

namespace detail {

AsyncProviderSetupExecution runAsyncProviderSetup(
    const AsyncProviderSetupApi& api,
    const WindowsMlProviderSetupProgress& reportProgress,
    const WindowsMlProviderSetupCancellation& cancellationRequested) {
  AsyncProviderSetupExecution execution;
  if (!api.start || !api.cancel || !api.getStatus || !api.close) {
    return execution;
  }

  AsyncSetupState asyncState;
  WinMLAsyncBlock async{};
  async.context = &asyncState;
  async.callback = providerSetupCompleted;
  async.progress = providerSetupProgress;

  struct AsyncLifetime {
    const AsyncProviderSetupApi& api;
    WinMLAsyncBlock& async;
    bool started = false;
    bool settled = false;

    ~AsyncLifetime() {
      try {
        if (started && !settled) {
          api.cancel(&async);
          api.getStatus(&async, TRUE);
        }
        api.close(&async);
      } catch (...) {
        // C ABI adapters are expected not to throw. Never let a defensive
        // cleanup path mask the original worker failure.
      }
    }
  } lifetime{api, async};

  execution.startResult = api.start(&async);
  lifetime.started = SUCCEEDED(execution.startResult);
  if (!lifetime.started) return execution;

  for (;;) {
    if (!execution.cancellationIssued && cancellationRequested &&
        cancellationRequested()) {
      const HRESULT cancelResult = api.cancel(&async);
      if (FAILED(cancelResult)) {
        execution.cancellationFailure = cancelResult;
      }
      execution.cancellationIssued = true;
    }

    double progress = 0.0;
    bool progressChanged = false;
    bool completed = false;
    {
      std::unique_lock<std::mutex> lock(asyncState.mutex);
      asyncState.changed.wait_for(lock, std::chrono::milliseconds(100), [&]() {
        return asyncState.completed || asyncState.progressChanged;
      });
      completed = asyncState.completed;
      progressChanged = asyncState.progressChanged;
      progress = asyncState.progress;
      asyncState.progressChanged = false;
    }
    if (progressChanged && reportProgress) {
      reportProgress(static_cast<float>(progress),
                     "Installing the optional NVIDIA audio component");
    }
    if (completed) break;
  }

  execution.statusResult = api.getStatus(&async, TRUE);
  lifetime.settled = true;
  return execution;
}

}  // namespace detail

WindowsMlBackendResolution resolveNvidiaWindowsMlBackend(
    ProviderProvisioningPolicy policy) {
  CatalogOwner catalog;
  MatchingProvider match;
  WindowsMlBackendResolution discoveryFailure;
  if (!discoverNvidiaProvider(&catalog, &match, &discoveryFailure)) {
    return discoveryFailure;
  }

  WindowsMlBackendResolution resolution;
  resolution.version = match.version;
  if (match.readyState == WinMLEpReadyState_NotPresent) {
    resolution.status = WindowsMlBackendStatus::InstallationRequired;
    resolution.detail =
        "The certified NVIDIA TensorRT-RTX provider is compatible but not "
        "installed.";
    return resolution;
  }
  if (match.readyState == WinMLEpReadyState_NotReady &&
      policy == ProviderProvisioningPolicy::ObserveOnly) {
    resolution.status = WindowsMlBackendStatus::Installed;
    resolution.detail =
        "The NVIDIA TensorRT-RTX provider is installed but is not active in "
        "this process.";
    return resolution;
  }
  if (match.readyState == WinMLEpReadyState_NotReady) {
    const HRESULT readyResult = WinMLEpEnsureReady(match.handle);
    if (FAILED(readyResult)) {
      return failedResolution(
          "Windows ML could not activate the installed NVIDIA provider (" +
          hresultDescription(readyResult) + ").");
    }
    WinMLEpReadyState updatedState = WinMLEpReadyState_NotReady;
    const HRESULT stateResult =
        WinMLEpGetReadyState(match.handle, &updatedState);
    if (FAILED(stateResult) || updatedState != WinMLEpReadyState_Ready) {
      return failedResolution(
          "Windows ML did not make the installed NVIDIA provider ready (" +
          hresultDescription(stateResult) + ").");
    }
  }
  return readyResolution(match);
}

WindowsMlProviderSetupResult setupNvidiaWindowsMlBackend(
    const WindowsMlProviderSetupProgress& reportProgress,
    const WindowsMlProviderSetupCancellation& cancellationRequested) {
  if (cancellationRequested && cancellationRequested()) {
    WindowsMlProviderSetupResult result;
    result.outcome = WindowsMlProviderSetupOutcome::Cancelled;
    return result;
  }

  CatalogOwner catalog;
  MatchingProvider match;
  WindowsMlBackendResolution discoveryFailure;
  if (!discoverNvidiaProvider(&catalog, &match, &discoveryFailure)) {
    WindowsMlProviderSetupResult result;
    result.outcome = WindowsMlProviderSetupOutcome::Failed;
    result.detail = discoveryFailure.detail;
    result.resolution = std::move(discoveryFailure);
    return result;
  }
  if (match.readyState == WinMLEpReadyState_Ready) {
    WindowsMlProviderSetupResult result;
    result.resolution = readyResolution(match);
    result.outcome = result.resolution.ready()
                         ? WindowsMlProviderSetupOutcome::Ready
                         : WindowsMlProviderSetupOutcome::Failed;
    result.detail = result.resolution.detail;
    if (result.ready() && reportProgress) {
      reportProgress(1.0f, "NVIDIA audio component is ready");
    }
    return result;
  }

  if (reportProgress) {
    reportProgress(0.0f,
                   match.readyState == WinMLEpReadyState_NotPresent
                       ? "Downloading the optional NVIDIA audio component"
                       : "Activating the NVIDIA audio component");
  }

  detail::AsyncProviderSetupApi asyncApi;
  asyncApi.start = [provider = match.handle](WinMLAsyncBlock* async) {
    return WinMLEpEnsureReadyAsync(provider, async);
  };
  asyncApi.cancel = WinMLAsyncCancel;
  asyncApi.getStatus = WinMLAsyncGetStatus;
  asyncApi.close = WinMLAsyncClose;
  const detail::AsyncProviderSetupExecution async =
      detail::runAsyncProviderSetup(asyncApi, reportProgress,
                                    cancellationRequested);
  if (!async.started()) {
    return failedSetup(
        "Windows ML could not start NVIDIA provider setup (" +
        hresultDescription(async.startResult) + ").");
  }
  if (FAILED(async.statusResult)) {
    if (async.cancellationIssued && !async.cancellationFailure) {
      WindowsMlProviderSetupResult result;
      result.outcome = WindowsMlProviderSetupOutcome::Cancelled;
      return result;
    }
    if (async.cancellationFailure) {
      return failedSetup(
          "Windows ML could not cancel NVIDIA provider setup (" +
          hresultDescription(*async.cancellationFailure) +
          "); setup then failed (" +
          hresultDescription(async.statusResult) + ").");
    }
    return failedSetup(
        "Windows ML could not install the NVIDIA provider (" +
        hresultDescription(async.statusResult) + ").");
  }

  WinMLEpReadyState readyState = WinMLEpReadyState_NotReady;
  const HRESULT stateResult = WinMLEpGetReadyState(match.handle, &readyState);
  if (FAILED(stateResult) || readyState != WinMLEpReadyState_Ready) {
    return failedSetup(
        "Windows ML completed setup without making the NVIDIA provider ready "
        "(" +
        hresultDescription(stateResult) + ").");
  }

  WindowsMlProviderSetupResult result;
  result.resolution = readyResolution(match);
  result.outcome = result.resolution.ready()
                       ? WindowsMlProviderSetupOutcome::Ready
                       : WindowsMlProviderSetupOutcome::Failed;
  result.detail = result.resolution.detail;
  if (result.ready() && reportProgress) {
    reportProgress(1.0f, "NVIDIA audio component is ready");
  }
  return result;
}

}  // namespace audio_separation
