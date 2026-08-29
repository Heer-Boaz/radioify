#include "audio/separation/windows_ml_backend.h"

#include <windows.h>

#include <WinMLEpCatalog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace audio_separation {
namespace {

constexpr std::string_view kNvidiaProviderName =
    "NvTensorRtRtxExecutionProvider";

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

  ~CatalogOwner() { WinMLEpCatalogRelease(handle); }
  CatalogOwner() = default;
  CatalogOwner(const CatalogOwner&) = delete;
  CatalogOwner& operator=(const CatalogOwner&) = delete;
};

struct MatchingProvider {
  WinMLEpHandle handle = nullptr;
  std::string name;
  std::string version;
  WinMLEpReadyState readyState = WinMLEpReadyState_NotPresent;
  WinMLEpCertification certification = WinMLEpCertification_Unknown;
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
  match->certification = info->certification;
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

std::string readProviderVersion(WinMLEpHandle provider) {
  size_t required = 0;
  if (FAILED(WinMLEpGetVersionSize(provider, &required)) || required == 0) {
    return {};
  }
  std::string version(required, '\0');
  size_t used = 0;
  if (FAILED(WinMLEpGetVersion(provider, version.size(), version.data(),
                               &used))) {
    return {};
  }
  if (used > 0 && used <= version.size()) version.resize(used);
  while (!version.empty() && version.back() == '\0') version.pop_back();
  return version;
}

WindowsMlBackendResolution failedResolution(std::string detail) {
  WindowsMlBackendResolution resolution;
  resolution.status = WindowsMlBackendStatus::Failed;
  resolution.detail = std::move(detail);
  return resolution;
}

}  // namespace

WindowsMlBackendResolution resolveNvidiaWindowsMlBackend(
    ProviderProvisioningPolicy policy) {
  CatalogOwner catalog;
  const HRESULT createResult = WinMLEpCatalogCreate(&catalog.handle);
  if (FAILED(createResult) || !catalog.handle) {
    WindowsMlBackendResolution resolution;
    resolution.status = WindowsMlBackendStatus::Unavailable;
    resolution.detail =
        "The Windows ML execution-provider catalog is unavailable (" +
        hresultDescription(createResult) + ").";
    return resolution;
  }

  MatchingProvider match;
  const HRESULT enumerateResult =
      WinMLEpCatalogEnumProviders(catalog.handle, findNvidiaProvider, &match);
  if (FAILED(enumerateResult)) {
    return failedResolution(
        "Windows ML could not enumerate execution providers (" +
        hresultDescription(enumerateResult) + ").");
  }
  if (!match.handle) {
    WindowsMlBackendResolution resolution;
    resolution.status = WindowsMlBackendStatus::Unavailable;
    resolution.detail =
        "Windows ML did not offer a compatible NVIDIA TensorRT-RTX provider.";
    return resolution;
  }

  WindowsMlBackendResolution resolution;
  resolution.version = match.version;
  if (match.certification != WinMLEpCertification_Certified) {
    resolution.status = WindowsMlBackendStatus::Unavailable;
    resolution.detail =
        "Windows ML offered an NVIDIA provider that is not certified.";
    return resolution;
  }
  if (match.readyState == WinMLEpReadyState_NotPresent) {
    if (policy != ProviderProvisioningPolicy::InstallIfMissing) {
      resolution.status = WindowsMlBackendStatus::InstallationRequired;
      resolution.detail =
          "The certified NVIDIA TensorRT-RTX provider is compatible but not "
          "installed.";
      return resolution;
    }
  }
  if (match.readyState == WinMLEpReadyState_NotReady &&
      policy == ProviderProvisioningPolicy::ObserveOnly) {
    resolution.status = WindowsMlBackendStatus::Installed;
    resolution.detail =
        "The NVIDIA TensorRT-RTX provider is installed but is not active in "
        "this process.";
    return resolution;
  }
  if (match.readyState == WinMLEpReadyState_NotReady ||
      match.readyState == WinMLEpReadyState_NotPresent) {
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

  std::filesystem::path libraryPath;
  if (!readLibraryPath(match.handle, &libraryPath, &resolution.detail)) {
    resolution.status = WindowsMlBackendStatus::Failed;
    return resolution;
  }
  if (const std::string activeVersion = readProviderVersion(match.handle);
      !activeVersion.empty()) {
    resolution.version = activeVersion;
  }
  InferenceBackend backend;
  backend.kind = InferenceBackendKind::WindowsMlNvidiaTensorRtRtx;
  backend.providerName = match.name;
  backend.displayName = "NVIDIA TensorRT-RTX";
  backend.diagnosticComponent = "nvidia-tensorrt-rtx";
  backend.providerLibrary = std::move(libraryPath);
  resolution.status = WindowsMlBackendStatus::Ready;
  resolution.backend = std::move(backend);
  resolution.detail = "The certified NVIDIA TensorRT-RTX provider is ready.";
  return resolution;
}

}  // namespace audio_separation
