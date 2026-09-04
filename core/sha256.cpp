#include "core/sha256.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <utility>
#include <vector>

namespace core_sha256 {
namespace {

void setError(std::string *error, std::string value) {
  if (error)
    *error = std::move(value);
}

struct AlgorithmHandle {
  BCRYPT_ALG_HANDLE value = nullptr;
  ~AlgorithmHandle() {
    if (value)
      BCryptCloseAlgorithmProvider(value, 0);
  }
};

struct HashHandle {
  BCRYPT_HASH_HANDLE value = nullptr;
  ~HashHandle() {
    if (value)
      BCryptDestroyHash(value);
  }
};

bool createHash(AlgorithmHandle *algorithm, HashHandle *hash,
                std::vector<unsigned char> *object, DWORD *digestLength,
                std::string *error) {
  if (!algorithm || !hash || !object || !digestLength)
    return false;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm->value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  DWORD objectLength = 0;
  DWORD bytesRead = 0;
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptGetProperty(
        algorithm->value, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &bytesRead, 0);
  }
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptGetProperty(
        algorithm->value, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(digestLength), sizeof(*digestLength),
        &bytesRead, 0);
  }
  if (!BCRYPT_SUCCESS(status) || objectLength == 0 || *digestLength == 0) {
    setError(error, "Could not initialize SHA-256.");
    return false;
  }
  object->resize(objectLength);
  status = BCryptCreateHash(algorithm->value, &hash->value, object->data(),
                            objectLength, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    setError(error, "Could not create a SHA-256 hash.");
    return false;
  }
  return true;
}

std::string finishHash(HashHandle &hash, DWORD digestLength,
                       std::string *error) {
  std::vector<unsigned char> bytes(digestLength);
  if (!BCRYPT_SUCCESS(
          BCryptFinishHash(hash.value, bytes.data(), digestLength, 0))) {
    setError(error, "Could not finish SHA-256.");
    return {};
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    encoded.push_back(kHex[byte >> 4]);
    encoded.push_back(kHex[byte & 0x0f]);
  }
  return encoded;
}

} // namespace

bool file(const std::filesystem::path &path,
          const std::function<bool()> &cancelled, std::uintmax_t *size,
          std::string *digest, std::string *error) {
  if (error)
    error->clear();
  if (!size || !digest) {
    setError(error, "No SHA-256 result destination was provided.");
    return false;
  }
  *size = 0;
  digest->clear();
  std::error_code fileError;
  if (!std::filesystem::is_regular_file(path, fileError) || fileError) {
    setError(error, "The file to verify is missing.");
    return false;
  }
  *size = std::filesystem::file_size(path, fileError);
  if (fileError) {
    setError(error, "Could not inspect the file to verify.");
    return false;
  }

  AlgorithmHandle algorithm;
  HashHandle hash;
  std::vector<unsigned char> object;
  DWORD digestLength = 0;
  if (!createHash(&algorithm, &hash, &object, &digestLength, error))
    return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    setError(error, "Could not open the file to verify.");
    return false;
  }
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    if (cancelled && cancelled()) {
      setError(error, "SHA-256 verification cancelled.");
      return false;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0)
      continue;
    if (!BCRYPT_SUCCESS(BCryptHashData(
            hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
            static_cast<ULONG>(count), 0))) {
      setError(error, "Could not hash the file.");
      return false;
    }
  }
  if (!input.eof()) {
    setError(error, "Could not finish reading the file to verify.");
    return false;
  }
  *digest = finishHash(hash, digestLength, error);
  return !digest->empty();
}

std::string text(const std::string &value) {
  AlgorithmHandle algorithm;
  HashHandle hash;
  std::vector<unsigned char> object;
  DWORD digestLength = 0;
  if (!createHash(&algorithm, &hash, &object, &digestLength, nullptr))
    return {};
  if (!value.empty() &&
      !BCRYPT_SUCCESS(BCryptHashData(
          hash.value,
          reinterpret_cast<PUCHAR>(const_cast<char *>(value.data())),
          static_cast<ULONG>(value.size()), 0))) {
    return {};
  }
  return finishHash(hash, digestLength, nullptr);
}

} // namespace core_sha256
