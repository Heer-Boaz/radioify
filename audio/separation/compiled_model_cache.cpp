#include "audio/separation/compiled_model_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "audio/separation/inference_session.h"
#include "core/file_output.h"
#include "core/runtime_helpers.h"

#ifndef RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256
#error RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256 must identify the verified model
#endif

namespace audio_separation {
namespace {

constexpr unsigned int kCacheSchemaVersion = 2;
constexpr std::uintmax_t kMinimumCompiledModelBytes = 1024 * 1024;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string safePathComponent(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '.' || character == '-' ||
        character == '_') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('_');
    }
  }
  return result;
}

bool usableCompiledModel(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return !error && size >= kMinimumCompiledModelBytes;
}

class CompilationMutex {
 public:
  ~CompilationMutex() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
  }

  CompilationMutex(const CompilationMutex&) = delete;
  CompilationMutex& operator=(const CompilationMutex&) = delete;

  static std::unique_ptr<CompilationMutex> acquire(
      std::wstring name, const ExecutionControl& control,
      std::string* error) {
    auto mutex = std::unique_ptr<CompilationMutex>(
        new CompilationMutex(CreateMutexW(nullptr, FALSE, name.c_str())));
    if (!mutex->handle_) {
      setError(error, "Could not create the audio-separation cache lock "
                      "(Windows error " +
                          std::to_string(GetLastError()) + ").");
      return nullptr;
    }
    for (;;) {
      if (!control.checkpoint()) {
        setError(error, "Audio separation cancelled.");
        return nullptr;
      }
      const DWORD waitResult = WaitForSingleObject(mutex->handle_, 100);
      if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        mutex->owned_ = true;
        return mutex;
      }
      if (waitResult != WAIT_TIMEOUT) {
        setError(error, "Could not wait for the audio-separation cache lock "
                        "(Windows error " +
                            std::to_string(GetLastError()) + ").");
        return nullptr;
      }
    }
  }

 private:
  explicit CompilationMutex(HANDLE handle) : handle_(handle) {}

  HANDLE handle_ = nullptr;
  bool owned_ = false;
};

std::wstring mutexName(std::string_view providerVersion) {
  std::string identity = RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256;
  identity.resize(std::min<std::size_t>(identity.size(), 16));
  identity += '-';
  identity += safePathComponent(providerVersion);
  return L"Local\\Radioify.AudioSeparation." +
         std::wstring(identity.begin(), identity.end());
}

}  // namespace

bool prepareBundledModel(
    const std::filesystem::path& sourceModelPath, InferenceBackend backend,
    const DiagnosticReporter& diagnostics, const ExecutionControl& control,
    ModelPreparationMode mode,
    const ModelPreparationReporter& reportPreparation,
    PreparedModel* prepared, std::string* error) {
  if (error) error->clear();
  if (!prepared) {
    setError(error, "No prepared-model destination was provided.");
    return false;
  }
  *prepared = {};
  if (backend.kind !=
      InferenceBackendKind::WindowsMlNvidiaTensorRtRtx) {
    prepared->modelPath = sourceModelPath;
    prepared->backend = std::move(backend);
    return true;
  }
  if (backend.providerVersion.empty()) {
    setError(error, "Windows ML did not report the NVIDIA provider version; "
                    "a safe compiled-model cache cannot be selected.");
    return false;
  }

  const std::string version = safePathComponent(backend.providerVersion);
  if (version.empty()) {
    setError(error, "The NVIDIA provider version cannot be used as a cache "
                    "identity.");
    return false;
  }
  const std::filesystem::path cacheDirectory =
      radioifyWritableDataDir() / "cache" /
      ("audio-separation-v" + std::to_string(kCacheSchemaVersion)) /
      RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256 / version;
  const std::filesystem::path runtimeCacheDirectory =
      cacheDirectory / "runtime";
  std::error_code directoryError;
  std::filesystem::create_directories(runtimeCacheDirectory, directoryError);
  if (directoryError ||
      !std::filesystem::is_directory(runtimeCacheDirectory, directoryError) ||
      directoryError) {
    setError(error, directoryError
                        ? "Could not create the NVIDIA audio-separation "
                          "cache: " +
                              directoryError.message()
                        : "The NVIDIA audio-separation cache path is not a "
                          "directory.");
    return false;
  }
  backend.setProviderOption("nv_runtime_cache_path",
                            toUtf8String(runtimeCacheDirectory));

  const std::filesystem::path compiledModelPath =
      cacheDirectory / "bandit-v2-tensorrt-rtx.ep.onnx";
  bool resumingCompilation = false;
  for (;;) {
    if (!control.checkpoint()) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    if (mode == ModelPreparationMode::ReuseOrCreate &&
        usableCompiledModel(compiledModelPath)) {
      reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model-cache",
                       "Using compiled NVIDIA model: " +
                           toUtf8String(compiledModelPath));
      prepared->modelPath = compiledModelPath;
      prepared->backend = std::move(backend);
      prepared->compiled = true;
      prepared->cacheHit = true;
      return true;
    }

    if (reportPreparation) {
      reportPreparation(resumingCompilation
                            ? "Waiting to resume NVIDIA model preparation"
                            : "Waiting to prepare the NVIDIA separation "
                              "model");
    }

    std::unique_ptr<CompilationMutex> lock = CompilationMutex::acquire(
        mutexName(backend.providerVersion), control, error);
    if (!lock) return false;
    if (mode == ModelPreparationMode::ReuseOrCreate &&
        usableCompiledModel(compiledModelPath)) {
      reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model-cache",
                       "Using compiled NVIDIA model created by another "
                       "Radioify process.");
      prepared->modelPath = compiledModelPath;
      prepared->backend = std::move(backend);
      prepared->compiled = true;
      prepared->cacheHit = true;
      return true;
    }

    bool lockYielded = false;
    if (!control.checkpoint([&]() {
          lock.reset();
          lockYielded = true;
        })) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    if (lockYielded) {
      resumingCompilation = true;
      continue;
    }

    reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model-cache",
                     mode == ModelPreparationMode::Rebuild
                         ? "Rebuilding the compiled NVIDIA separation model."
                         : "Compiling the NVIDIA separation model for first "
                           "use.");
    if (reportPreparation) {
      std::string phase;
      if (mode == ModelPreparationMode::Rebuild) {
        phase = "Repairing the NVIDIA separation model cache";
      } else if (resumingCompilation) {
        phase = "Resuming NVIDIA model optimization (first use)";
      } else {
        phase = "Optimizing the NVIDIA separation model (first use)";
      }
      reportPreparation(std::move(phase));
    }
    std::optional<file_output::Transaction> transaction =
        file_output::Transaction::begin(
            compiledModelPath, file_output::PublishMode::ReplaceExisting,
            error);
    if (!transaction) return false;

    std::unique_ptr<InferenceSessionFactory> compiler =
        InferenceSessionFactory::create(backend, diagnostics, error);
    if (!compiler) return false;
    const ControlledOperationResult compilation = compiler->compileModel(
        sourceModelPath, transaction->temporaryPath(), control);
    if (std::holds_alternative<OperationInterrupted>(compilation)) {
      if (control.cancellationRequested()) {
        setError(error, "Audio separation cancelled.");
        return false;
      }
      reportDiagnostic(diagnostics, DiagnosticLevel::Info, "scheduler",
                       "NVIDIA model compilation was interrupted for "
                       "foreground playback.");
      compiler.reset();
      transaction.reset();
      lock.reset();
      if (!control.checkpoint()) {
        setError(error, "Audio separation cancelled.");
        return false;
      }
      resumingCompilation = true;
      continue;
    }
    if (const auto* failure =
            std::get_if<OperationFailure>(&compilation)) {
      setError(error, "Could not compile the NVIDIA separation model: " +
                          failure->detail);
      return false;
    }
    compiler.reset();
    bool publicationLockYielded = false;
    if (!control.checkpoint([&]() {
          transaction.reset();
          lock.reset();
          publicationLockYielded = true;
        })) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    if (publicationLockYielded) {
      resumingCompilation = true;
      continue;
    }
    if (!transaction->publish(error)) return false;
    reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model-cache",
                     "Published compiled NVIDIA model: " +
                         toUtf8String(compiledModelPath));
    prepared->modelPath = compiledModelPath;
    prepared->backend = std::move(backend);
    prepared->compiled = true;
    prepared->cacheHit = false;
    return true;
  }
}

}  // namespace audio_separation
