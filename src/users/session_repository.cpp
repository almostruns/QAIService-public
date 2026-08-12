#include "users/session_repository.h"

#include "db/mysql_connection.h"
#include "db/mysql_statement.h"

#include <mysql.h>

#include <array>

namespace qaiservice::users {

SessionRepository::SessionRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

void SessionRepository::create(const std::string& token_hash, std::uint64_t user_id, std::int64_t created_at_ms,
                               std::int64_t expires_at_ms)
{
  constexpr char sql[] = "INSERT INTO sessions (token_hash, user_id, created_at_ms, expires_at_ms) VALUES (?, ?, ?, ?)";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  std::array<MYSQL_BIND, 4> parameters{};
  unsigned long hash_length = 0;
  db::bindString(parameters[0], token_hash, hash_length);
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &user_id;
  parameters[1].is_unsigned = true;
  parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[2].buffer = &created_at_ms;
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &expires_at_ms;

  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot insert session");
  }
}

std::vector<PersistedSession> SessionRepository::loadValid(std::int64_t now_ms)
{
  constexpr char sql[] = "SELECT token_hash, user_id, expires_at_ms FROM sessions WHERE expires_at_ms > ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_LONGLONG;
  parameter.buffer = &now_ms;

  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot query sessions");
  }

  std::uint64_t user_id = 0;
  std::int64_t expires_at_ms = 0;
  std::array<char, 65> hash_buffer{};
  unsigned long hash_length = 0;
  std::array<my_bool, 3> is_null{};
  std::array<my_bool, 3> errors{};
  std::array<MYSQL_BIND, 3> results{};

  results[0].buffer_type = MYSQL_TYPE_STRING;
  results[0].buffer = hash_buffer.data();
  results[0].buffer_length = static_cast<unsigned long>(hash_buffer.size());
  results[0].length = &hash_length;
  results[0].is_null = &is_null[0];
  results[0].error = &errors[0];
  results[1].buffer_type = MYSQL_TYPE_LONGLONG;
  results[1].buffer = &user_id;
  results[1].is_unsigned = true;
  results[1].is_null = &is_null[1];
  results[1].error = &errors[1];
  results[2].buffer_type = MYSQL_TYPE_LONGLONG;
  results[2].buffer = &expires_at_ms;
  results[2].is_null = &is_null[2];
  results[2].error = &errors[2];

  if (mysql_stmt_bind_result(statement.get(), results.data()) != 0 || mysql_stmt_store_result(statement.get()) != 0) {
    throw db::DatabaseError("cannot read session query result");
  }

  std::vector<PersistedSession> sessions;
  while (true) {
    const int fetch_status = mysql_stmt_fetch(statement.get());
    if (fetch_status == MYSQL_NO_DATA) {
      break;
    }
    if (fetch_status != 0 || errors[0] || errors[1] || errors[2] || is_null[0] || is_null[1] || is_null[2]) {
      throw db::DatabaseError("invalid session query result");
    }
    sessions.push_back(PersistedSession{std::string(hash_buffer.data(), hash_length), user_id, expires_at_ms});
  }
  return sessions;
}

void SessionRepository::erase(const std::string& token_hash)
{
  constexpr char sql[] = "DELETE FROM sessions WHERE token_hash = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  MYSQL_BIND parameter{};
  unsigned long hash_length = 0;
  db::bindString(parameter, token_hash, hash_length);

  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot delete session");
  }
}

std::uint64_t SessionRepository::eraseExpired(std::int64_t now_ms)
{
  constexpr char sql[] = "DELETE FROM sessions WHERE expires_at_ms <= ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_LONGLONG;
  parameter.buffer = &now_ms;

  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot purge expired sessions");
  }
  return static_cast<std::uint64_t>(mysql_stmt_affected_rows(statement.get()));
}

}  // namespace qaiservice::users
