#include "audio/separation/artifact.h"
#include "audio/separation/mask_model.h"
#include "audio/separation/spectral_transform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int parseIterations(const wchar_t* value) {
  if (!value) return 0;
  wchar_t* end = nullptr;
  const long parsed = std::wcstol(value, &end, 10);
  if (!end || *end != L'\0' || parsed <= 0 || parsed > 100000) return 0;
  return static_cast<int>(parsed);
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
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: audio_separation_model_stress <model-file> "
                 "[iterations]\n";
    return 2;
  }
  const int iterations = argc == 3 ? parseIterations(argv[2]) : 256;
  if (iterations <= 0) {
    std::cerr << "Iterations must be between 1 and 100000.\n";
    return 2;
  }

  audio_separation::BanditMaskModel model;
  std::string error;
  if (!model.initialize(
          std::filesystem::path(argv[1]),
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
  std::span<const float> output;
  const auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    if (!model.run(input, &output, &cancelRequested, &error)) {
      std::cerr << "Inference " << (iteration + 1)
                << " failed: " << error << '\n';
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
    if ((iteration + 1) % 16 == 0 || iteration + 1 == iterations) {
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::cout << (iteration + 1) << '/' << iterations << " in "
                << elapsed << " s\n";
    }
  }
  return EXIT_SUCCESS;
}
