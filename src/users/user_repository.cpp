#include "users/user_repository.h"

#include "db/mysql_statement.h"
#include "db/mysql_connection.h"

#include <mysql.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

namespace qaiservice::users {
namespace {

constexpr unsigned int kDuplicateEntryError = 1062;

}  // namespace

UserRepository::UserRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

CreateUserResult UserRepository::create(const std::string& username, const std::string& password_hash)
{
  constexpr char sql[] = "INSERT INTO users (username, password_hash) VALUES (?, ?)";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  std::array<MYSQL_BIND, 2> parameters{};
  std::array<unsigned long, 2> lengths{};
  db::bindString(parameters[0], username, lengths[0]);
  db::bindString(parameters[1], password_hash, lengths[1]);

  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0) {
    throw db::DatabaseError("cannot bind user insert parameters");
  }

  if (mysql_stmt_execute(statement.get()) != 0) {
    if (mysql_stmt_errno(statement.get()) == kDuplicateEntryError) {
      return {CreateUserStatus::kUsernameExists, std::nullopt};
    }
    throw db::DatabaseError("cannot insert user");
  }

  const auto user_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(statement.get()));
  return {CreateUserStatus::kCreated, User{user_id, username, password_hash}};
}

std::optional<User> UserRepository::findByUsername(std::string_view username_view)
{
  constexpr char sql[] = "SELECT id, username, password_hash FROM users WHERE username = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  const std::string username(username_view);

  MYSQL_BIND parameter{};
  unsigned long parameter_length = 0;
  db::bindString(parameter, username, parameter_length);

  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot query user");
  }

  std::uint64_t user_id = 0;
  std::array<char, 65> username_buffer{};
  std::array<char, 256> hash_buffer{};
  std::array<unsigned long, 3> lengths{};
  std::array<my_bool, 3> is_null{};
  std::array<my_bool, 3> errors{};
  std::array<MYSQL_BIND, 3> results{};

  results[0].buffer_type = MYSQL_TYPE_LONGLONG;
  results[0].buffer = &user_id;
  results[0].is_unsigned = true;
  results[0].is_null = &is_null[0];
  results[0].error = &errors[0];

  results[1].buffer_type = MYSQL_TYPE_STRING;
  results[1].buffer = username_buffer.data();
  results[1].buffer_length = static_cast<unsigned long>(username_buffer.size());
  results[1].length = &lengths[1];
  results[1].is_null = &is_null[1];
  results[1].error = &errors[1];

  results[2].buffer_type = MYSQL_TYPE_STRING;
  results[2].buffer = hash_buffer.data();
  results[2].buffer_length = static_cast<unsigned long>(hash_buffer.size());
  results[2].length = &lengths[2];
  results[2].is_null = &is_null[2];
  results[2].error = &errors[2];

  if (mysql_stmt_bind_result(statement.get(), results.data()) != 0 || mysql_stmt_store_result(statement.get()) != 0) {
    throw db::DatabaseError("cannot read user query result");
  }

  const int fetch_status = mysql_stmt_fetch(statement.get());
  if (fetch_status == MYSQL_NO_DATA) {
    return std::nullopt;
  }

  if (fetch_status != 0 || errors[0] || errors[1] || errors[2] || is_null[0] || is_null[1] || is_null[2]) {
    throw db::DatabaseError("invalid user query result");
  }

  User user;
  user.id = user_id;
  user.username.assign(username_buffer.data(), lengths[1]);
  user.password_hash.assign(hash_buffer.data(), lengths[2]);
  return user;
}

}  // namespace qaiservice::users
