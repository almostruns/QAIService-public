#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::users {

struct User {
  std::uint64_t id;
  std::string username;
  std::string password_hash;
};

enum class CreateUserStatus {
  kCreated,
  kUsernameExists,
};

struct CreateUserResult {
  CreateUserStatus status;
  std::optional<User> user;
};

class UserRepository {
 public:
  explicit UserRepository(db::MySqlConnection& connection);

  [[nodiscard]] CreateUserResult create(const std::string& username, const std::string& password_hash);
  [[nodiscard]] std::optional<User> findByUsername(std::string_view username);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::users
