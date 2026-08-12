#include "users/session_store.h"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace qaiservice::users {
namespace {

constexpr std::size_t kCreateCleanupLimit = 64;
constexpr std::size_t kRequestCleanupLimit = 16;

template <std::size_t N>
std::string toHex(const std::array<unsigned char, N>& bytes)
{
  constexpr char hex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const unsigned char value : bytes) {
    encoded.push_back(hex[value >> 4]);
    encoded.push_back(hex[value & 0x0f]);
  }
  return encoded;
}

}  // namespace

std::string hashSessionToken(std::string_view token)
{
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(token.data()), token.size());
  return toHex(digest);
}

SessionStore::SessionStore(std::chrono::seconds lifetime) : SessionStore(lifetime, [] {
  return Clock::now();
})
{
}

SessionStore::SessionStore(std::chrono::seconds lifetime, NowFunction now)
    : lifetime_(lifetime), now_(std::move(now))
{
  if (sodium_init() < 0) {
    throw std::runtime_error("cannot initialize session randomness");
  }
}

Session SessionStore::create(std::uint64_t user_id)
{
  const Clock::time_point now = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now, kCreateCleanupLimit);

  std::string token = generateToken();
  std::string token_hash = hashSessionToken(token);
  while (sessions_.count(token_hash) != 0) {
    token = generateToken();
    token_hash = hashSessionToken(token);
  }

  Session session{std::move(token), user_id, now + lifetime_};
  ExpiryIndex::iterator expiry = expiry_index_.emplace(session.expires_at, token_hash);
  sessions_.emplace(std::move(token_hash), StoredSession{session, expiry});
  return session;
}

void SessionStore::loadPersisted(std::string token_hash, std::uint64_t user_id, Clock::time_point expires_at)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (sessions_.count(token_hash) != 0) {
    return;
  }

  Session session{"", user_id, expires_at};
  ExpiryIndex::iterator expiry = expiry_index_.emplace(expires_at, token_hash);
  sessions_.emplace(std::move(token_hash), StoredSession{std::move(session), expiry});
}

std::optional<std::uint64_t> SessionStore::findUserId(std::string_view token_view)
{
  const std::string token_hash = hashSessionToken(token_view);
  const Clock::time_point now = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now, kRequestCleanupLimit);
  const auto session = sessions_.find(token_hash);

  if (session == sessions_.end()) {
    return std::nullopt;
  }

  if (session->second.session.expires_at <= now) {
    expiry_index_.erase(session->second.expiry);
    sessions_.erase(session);
    return std::nullopt;
  }

  return session->second.session.user_id;
}

std::size_t SessionStore::size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

void SessionStore::erase(std::string_view token)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto session = sessions_.find(hashSessionToken(token));
  if (session == sessions_.end()) {
    return;
  }
  expiry_index_.erase(session->second.expiry);
  sessions_.erase(session);
}

void SessionStore::removeExpired(Clock::time_point now, std::size_t limit)
{
  std::size_t removed = 0;
  while (removed < limit && !expiry_index_.empty() && expiry_index_.begin()->first <= now) {
    ExpiryIndex::iterator expiry = expiry_index_.begin();
    sessions_.erase(expiry->second);
    expiry_index_.erase(expiry);
    ++removed;
  }
}

std::string SessionStore::generateToken()
{
  std::array<unsigned char, 32> random_bytes{};
  randombytes_buf(random_bytes.data(), random_bytes.size());
  return toHex(random_bytes);
}

}  // namespace qaiservice::users
