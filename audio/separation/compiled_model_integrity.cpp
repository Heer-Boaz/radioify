#include "audio/separation/compiled_model_integrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace audio_separation {
namespace {

constexpr const char* kManifestMagic =
    "radioify-compiled-model-integrity";
constexpr unsigned int kManifestSchemaVersion = 1;
constexpr std::uintmax_t kMinimumCompiledModelBytes = 1024 * 1024;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string cngError(const char* operation, NTSTATUS status) {
  return std::string(operation) + " failed (CNG status " +
         std::to_string(static_cast<long>(status)) + ").";
}

struct AlgorithmHandle {
  BCRYPT_ALG_HANDLE value = nullptr;
  ~AlgorithmHandle() {
    if (value) BCryptCloseAlgorithmProvider(value, 0);
  }
};

struct HashHandle {
  BCRYPT_HASH_HANDLE value = nullptr;
  ~HashHandle() {
    if (value) BCryptDestroyHash(value);
  }
};

bool sha256File(const std::filesystem::path& path, std::uintmax_t* size,
                std::string* digest, std::string* error) {
  if (error) error->clear();
  if (size) *size = 0;
  if (digest) digest->clear();
  if (!size || !digest) {
    setError(error, "No compiled-model digest destination was provided.");
    return false;
  }

  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    setError(error, "The compiled model is not a readable file.");
    return false;
  }
  const std::uintmax_t fileSize =
      std::filesystem::file_size(path, filesystemError);
  if (filesystemError) {
    setError(error, "Could not inspect the compiled model: " +
                        filesystemError.message());
    return false;
  }
  if (fileSize < kMinimumCompiledModelBytes) {
    setError(error, "The compiled model is smaller than the minimum valid "
                    "cache artifact.");
    return false;
  }

  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    setError(error, cngError("Opening SHA-256", status));
    return false;
  }

  DWORD objectLength = 0;
  DWORD hashLength = 0;
  DWORD bytesRead = 0;
  status = BCryptGetProperty(
      algorithm.value, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
      &bytesRead, 0);
  if (!BCRYPT_SUCCESS(status) || objectLength == 0) {
    setError(error, cngError("Reading SHA-256 object size", status));
    return false;
  }
  status = BCryptGetProperty(
      algorithm.value, BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
      &bytesRead, 0);
  if (!BCRYPT_SUCCESS(status) || hashLength == 0) {
    setError(error, cngError("Reading SHA-256 digest size", status));
    return false;
  }

  std::vector<unsigned char> hashObject(objectLength);
  HashHandle hash;
  status = BCryptCreateHash(algorithm.value, &hash.value, hashObject.data(),
                            objectLength, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    setError(error, cngError("Creating SHA-256", status));
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    setError(error, "Could not open the compiled model for hashing.");
    return false;
  }
  std::vector<char> buffer(256 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0) continue;
    status = BCryptHashData(
        hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
        static_cast<ULONG>(count), 0);
    if (!BCRYPT_SUCCESS(status)) {
      setError(error, cngError("Hashing the compiled model", status));
      return false;
    }
  }
  if (!input.eof()) {
    setError(error, "Could not finish reading the compiled model.");
    return false;
  }

  std::vector<unsigned char> hashBytes(hashLength);
  status = BCryptFinishHash(hash.value, hashBytes.data(), hashLength, 0);
  if (!BCRYPT_SUCCESS(status)) {
    setError(error, cngError("Finishing SHA-256", status));
    return false;
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(hashBytes.size() * 2);
  for (const unsigned char byte : hashBytes) {
    encoded.push_back(kHex[byte >> 4]);
    encoded.push_back(kHex[byte & 0x0f]);
  }
  *size = fileSize;
  *digest = std::move(encoded);
  return true;
}

}  // namespace

bool writeCompiledModelIntegrityManifest(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& manifestPath, std::string* error) {
  if (error) error->clear();
  std::uintmax_t size = 0;
  std::string digest;
  if (!sha256File(modelPath, &size, &digest, error)) return false;

  std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create the compiled-model integrity manifest.");
    return false;
  }
  output << kManifestMagic << ' ' << kManifestSchemaVersion << '\n'
         << size << '\n'
         << digest << '\n';
  output.flush();
  if (!output) {
    setError(error, "Could not finish the compiled-model integrity manifest.");
    return false;
  }
  return true;
}

bool verifyCompiledModelIntegrity(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& manifestPath, std::string* error) {
  if (error) error->clear();
  std::ifstream input(manifestPath, std::ios::binary);
  std::string magic;
  unsigned int schemaVersion = 0;
  std::uintmax_t expectedSize = 0;
  std::string expectedDigest;
  std::string trailing;
  if (!input || !(input >> magic >> schemaVersion >> expectedSize >>
                  expectedDigest) ||
      (input >> trailing) || magic != kManifestMagic ||
      schemaVersion != kManifestSchemaVersion ||
      expectedDigest.size() != 64) {
    setError(error, "The compiled-model integrity manifest is missing or "
                    "invalid.");
    return false;
  }

  std::uintmax_t actualSize = 0;
  std::string actualDigest;
  if (!sha256File(modelPath, &actualSize, &actualDigest, error)) return false;
  if (actualSize != expectedSize || actualDigest != expectedDigest) {
    setError(error, "The compiled model does not match its integrity "
                    "manifest.");
    return false;
  }
  return true;
}

}  // namespace audio_separation
