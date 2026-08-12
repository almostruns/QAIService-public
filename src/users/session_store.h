#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qaiservice::users {

// 登录态寿命：服务端过期时间与 Cookie Max-Age 都从它派生，避免两处漂移。
inline constexpr std::chrono::seconds kSessionLifetime{30 * 24 * 3600};

struct Session {
  std::string token;
  std::uint64_t user_id;
  std::chrono::system_clock::time_point expires_at;
};

// 持久化与内存索引只使用 token 的 SHA-256 哈希；明文只出现在 Set-Cookie 响应里。
[[nodiscard]] std::string hashSessionToken(std::string_view token);

class SessionStore {
 public:
  using Clock = std::chrono::system_clock;
  using NowFunction = std::function<Clock::time_point()>;

  explicit SessionStore(std::chrono::seconds lifetime);
  SessionStore(std::chrono::seconds lifetime, NowFunction now);

  [[nodiscard]] Session create(std::uint64_t user_id);
  void loadPersisted(std::string token_hash, std::uint64_t user_id, Clock::time_point expires_at);
  [[nodiscard]] std::optional<std::uint64_t> findUserId(std::string_view token);
  [[nodiscard]] std::size_t size() const;
  void erase(std::string_view token);

 private:
  using ExpiryIndex = std::multimap<Clock::time_point, std::string>;

  struct StoredSession {
    Session session;
    ExpiryIndex::iterator expiry;
  };

  [[nodiscard]] static std::string generateToken();
  void removeExpired(Clock::time_point now, std::size_t limit);

  std::chrono::seconds lifetime_;
  NowFunction now_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, StoredSession> sessions_;
  ExpiryIndex expiry_index_;
};

}  // namespace qaiservice::users
