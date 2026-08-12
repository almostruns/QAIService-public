#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::users {

struct PersistedSession {
  std::string token_hash;
  std::uint64_t user_id;
  std::int64_t expires_at_ms;
};

// sessions 表只存 token 的 SHA-256 哈希：数据库泄露时攻击者拿不到可用凭证，
// 与密码哈希是同一威胁模型。
class SessionRepository {
 public:
  explicit SessionRepository(db::MySqlConnection& connection);

  void create(const std::string& token_hash, std::uint64_t user_id, std::int64_t created_at_ms,
              std::int64_t expires_at_ms);
  [[nodiscard]] std::vector<PersistedSession> loadValid(std::int64_t now_ms);
  void erase(const std::string& token_hash);
  [[nodiscard]] std::uint64_t eraseExpired(std::int64_t now_ms);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::users
