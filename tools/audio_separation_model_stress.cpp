#include "audio/separation/artifact.h"
#include "audio/separation/mask_model.h"
#include "audio/separation/spectral_transform.h"
#include "audio/separation/windows_ml_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int parseIterations(const wchar_t* value) {
  if (!value) return 0;
  wchar_t* end = nullptr;
  const long parsed = std::wcstol(value, &end, 10);
  if (!end || *end != L'\0' || parsed <= 0 || parsed > 100000) return 0;
  return static_cast<int>(parsed);
}

struct CommandLine {
  std::filesystem::path modelPath;
  int iterations = 256;
  bool useNvidiaWindowsMl = false;
};

bool parseCommandLine(int argc, wchar_t** argv, CommandLine* options) {
  if (!options || argc < 2) return false;
  options->modelPath = argv[1];
  bool acceptedLegacyIterations = false;
  for (int index = 2; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--backend" && index + 1 < argc) {
      const std::wstring_view backend(argv[++index]);
      if (backend == L"directml") {
        options->useNvidiaWindowsMl = false;
      } else if (backend == L"windows-ml-nvidia") {
        options->useNvidiaWindowsMl = true;
      } else {
        return false;
      }
      continue;
    }
    if (argument == L"--iterations" && index + 1 < argc) {
      options->iterations = parseIterations(argv[++index]);
      if (options->iterations <= 0) return false;
      continue;
    }
    if (!acceptedLegacyIterations) {
      const int iterations = parseIterations(argv[index]);
      if (iterations > 0) {
        options->iterations = iterations;
        acceptedLegacyIterations = true;
        continue;
      }
    }
    return false;
  }
  return !options->modelPath.empty();
}

const char* backendStatusName(
    audio_separation::WindowsMlBackendStatus status) {
  using Status = audio_separation::WindowsMlBackendStatus;
  switch (status) {
    case Status::Ready:
      return "ready";
    case Status::Installed:
      return "installed";
    case Status::InstallationRequired:
      return "installation-required";
    case Status::Unavailable:
      return "unavailable";
    case Status::Failed:
      return "failed";
  }
  return "unknown";
}

const char* levelName(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::Info:
      return "info";
    case DiagnosticLevel::Warning:
      return "warning";
    case DiagnosticLevel::Error:
      return "error";
  }
  return "info";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  CommandLine commandLine;
  if (!parseCommandLine(argc, argv, &commandLine)) {
    std::cerr << "Usage: audio_separation_model_stress <model-file> "
                 "[iterations] [--iterations N] "
                 "[--backend directml|windows-ml-nvidia]\n";
    return 2;
  }

  audio_separation::BanditMaskModel model;
  audio_separation::InferenceBackend backend;
  if (commandLine.useNvidiaWindowsMl) {
    audio_separation::WindowsMlBackendResolution resolution =
        audio_separation::resolveNvidiaWindowsMlBackend(
            audio_separation::InstalledProviderPolicy::Activate);
    if (!resolution.ready()) {
      std::cerr << "NVIDIA Windows ML backend is "
                << backendStatusName(resolution.status) << ": "
                << resolution.detail << '\n';
      return EXIT_FAILURE;
    }
    std::cerr << "Using " << resolution.backend.displayName;
    if (!resolution.version.empty()) {
      std::cerr << " " << resolution.version;
    }
    std::cerr << ".\n";
    backend = std::move(resolution.backend);
  } else {
    backend = audio_separation::directMlInferenceBackend();
  }
  std::string error;
  if (!model.initialize(
          commandLine.modelPath, backend,
          [](DiagnosticLevel level, std::string_view component,
             std::string_view message) {
            std::cerr << '[' << levelName(level) << ':' << component << "] "
                      << message << '\n';
          },
          &error)) {
    std::cerr << "Model initialization failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  std::vector<float> input(
      audio_separation::BanditMaskModel::kBatchSize *
      audio_separation::BanditSpectralTransform::kRealImagValues);
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<float>(
        0.01 * std::sin(static_cast<double>(index % 4096) * 0.013));
  }
  std::atomic<bool> cancelRequested{false};
  const audio_separation::ExecutionControl control(&cancelRequested);
  std::span<const float> output;
  const auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < commandLine.iterations; ++iteration) {
    if (model.run(input, &output, control, &error) !=
        audio_separation::MaskInferenceResult::Succeeded) {
      std::cerr << "Inference " << (iteration + 1) << " failed: " << error
                << '\n';
      return EXIT_FAILURE;
    }
    const std::size_t expected =
        audio_separation::BanditMaskModel::kBatchSize *
        audio_separation::kStemCount *
        audio_separation::BanditSpectralTransform::kRealImagValues;
    if (output.size() != expected ||
        !std::all_of(output.begin(), output.end(),
                     [](float value) { return std::isfinite(value); })) {
      std::cerr << "Inference " << (iteration + 1)
                << " returned an invalid mask tensor.\n";
      return EXIT_FAILURE;
    }
    if ((iteration + 1) % 16 == 0 ||
        iteration + 1 == commandLine.iterations) {
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::cout << (iteration + 1) << '/' << commandLine.iterations << " in "
                << elapsed << " s\n";
    }
  }
  long double sum = 0.0;
  long double sumOfSquares = 0.0;
  long double weightedSum = 0.0;
  const auto [minimum, maximum] =
      std::minmax_element(output.begin(), output.end());
  for (std::size_t index = 0; index < output.size(); ++index) {
    const long double value = output[index];
    sum += value;
    sumOfSquares += value * value;
    weightedSum += value * static_cast<long double>((index % 1021) + 1);
  }
  const long double count = static_cast<long double>(output.size());
  std::cout << std::setprecision(17) << "Output min: " << *minimum << '\n'
            << "Output max: " << *maximum << '\n'
            << "Output mean: " << static_cast<double>(sum / count) << '\n'
            << "Output RMS: "
            << static_cast<double>(std::sqrt(sumOfSquares / count)) << '\n'
            << "Output weighted sum: " << static_cast<double>(weightedSum)
            << '\n';
  return EXIT_SUCCESS;
}
