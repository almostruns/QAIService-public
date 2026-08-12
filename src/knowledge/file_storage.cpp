#include "knowledge/file_storage.h"

#include <sodium.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace qaiservice::knowledge {
namespace {

std::string randomStorageKey()
{
  std::array<unsigned char, 16> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  std::array<char, 33> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
  return encoded.data();
}

std::string sha256(std::string_view bytes)
{
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
  return encoded.data();
}

bool validStorageKey(std::string_view key)
{
  if (key.size() != 32) {
    return false;
  }
  for (const char value : key) {
    const bool digit = value >= '0' && value <= '9';
    const bool lower_hex = value >= 'a' && value <= 'f';
    if (!digit && !lower_hex) {
      return false;
    }
  }
  return true;
}

void writeAll(int descriptor, std::string_view bytes)
{
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (result < 0) {
      throw std::runtime_error("cannot write stored file: " + std::string(std::strerror(errno)));
    }
    written += static_cast<std::size_t>(result);
  }
}

}  // namespace

FileStorage::FileStorage(std::filesystem::path root) : root_(std::move(root))
{
  if (root_.empty()) {
    throw std::invalid_argument("file storage root must not be empty");
  }
  if (sodium_init() < 0) {
    throw std::runtime_error("cannot initialize file storage cryptography");
  }

  std::filesystem::create_directories(root_);
  std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  root_ = std::filesystem::weakly_canonical(root_);
}

StoredFile FileStorage::store(std::uint64_t user_id, std::string_view original_name, std::string_view bytes) const
{
  if (original_name.empty()) {
    throw std::invalid_argument("original filename must not be empty");
  }
  if (bytes.empty()) {
    throw std::invalid_argument("stored file must not be empty");
  }

  const std::filesystem::path directory = userDirectory(user_id);
  std::filesystem::create_directories(directory);
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

  const std::string storage_key = randomStorageKey();
  const std::filesystem::path destination = directory / storage_key;
  const std::filesystem::path temporary = directory / (storage_key + ".tmp");
  const int descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::runtime_error("cannot create stored file: " + std::string(std::strerror(errno)));
  }

  try {
    writeAll(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      throw std::runtime_error("cannot flush stored file: " + std::string(std::strerror(errno)));
    }
    if (::close(descriptor) != 0) {
      throw std::runtime_error("cannot close stored file: " + std::string(std::strerror(errno)));
    }
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
      throw std::runtime_error("cannot publish stored file: " + std::string(std::strerror(errno)));
    }
  } catch (...) {
    ::close(descriptor);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }

  return {storage_key, sha256(bytes), static_cast<std::uint64_t>(bytes.size())};
}

std::filesystem::path FileStorage::pathFor(std::uint64_t user_id, std::string_view storage_key) const
{
  if (!validStorageKey(storage_key)) {
    throw std::invalid_argument("invalid storage key");
  }
  return userDirectory(user_id) / std::string(storage_key);
}

void FileStorage::remove(std::uint64_t user_id, std::string_view storage_key) const
{
  std::error_code error;
  std::filesystem::remove(pathFor(user_id, storage_key), error);
  if (error) {
    throw std::runtime_error("cannot remove stored file: " + error.message());
  }
}

std::filesystem::path FileStorage::userDirectory(std::uint64_t user_id) const
{
  if (user_id == 0) {
    throw std::invalid_argument("user id must be positive");
  }
  return root_ / std::to_string(user_id);
}

}  // namespace qaiservice::knowledge
