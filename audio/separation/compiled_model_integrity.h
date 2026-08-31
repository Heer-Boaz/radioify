#pragma once

#include <filesystem>
#include <string>

namespace audio_separation {

// Writes a versioned SHA-256 manifest for a compiled model to a caller-owned
// staging path. The cache owner publishes model and manifest as one
// TransactionGroup.
bool writeCompiledModelIntegrityManifest(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& manifestPath, std::string* error);

// A cache hit requires both the minimum compiled-model size and an exact
// digest match with its manifest. This detects interrupted writes and later
// disk corruption before ONNX Runtime sees the artifact.
bool verifyCompiledModelIntegrity(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& manifestPath, std::string* error = nullptr);

}  // namespace audio_separation
