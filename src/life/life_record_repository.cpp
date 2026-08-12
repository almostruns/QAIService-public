#include "life/life_record_repository.h"

#include "db/mysql_connection.h"
#include "db/mysql_statement.h"

#include <mysql.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::life {
namespace {

struct Result {
  std::uint64_t id{0};
  std::uint64_t user_id{0};
  std::array<char, 33> domain{};
  std::vector<char> payload = std::vector<char>(65537);
  std::int64_t occurred_at_ms{0};
  unsigned long domain_length{0};
  unsigned long payload_length{0};
  my_bool payload_error{false};
  std::array<MYSQL_BIND, 5> bindings{};

  Result()
  {
    bindings[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[0].buffer = &id;
    bindings[0].is_unsigned = true;
    bindings[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[1].buffer = &user_id;
    bindings[1].is_unsigned = true;
    bindings[2].buffer_type = MYSQL_TYPE_STRING;
    bindings[2].buffer = domain.data();
    bindings[2].buffer_length = domain.size();
    bindings[2].length = &domain_length;
    bindings[3].buffer_type = MYSQL_TYPE_STRING;
    bindings[3].buffer = payload.data();
    bindings[3].buffer_length = payload.size();
    bindings[3].length = &payload_length;
    bindings[3].error = &payload_error;
    bindings[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[4].buffer = &occurred_at_ms;
  }

  [[nodiscard]] LifeRecord record() const
  {
    if (payload_error) {
      throw db::DatabaseError("life record payload is too large");
    }
    return {id, user_id, std::string(domain.data(), domain_length),
            nlohmann::json::parse(payload.data(), payload.data() + payload_length), occurred_at_ms};
  }
};

}  // namespace

LifeRecordRepository::LifeRecordRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

CreateLifeRecordResult LifeRecordRepository::create(std::uint64_t user_id, const std::string& domain,
                                                    const nlohmann::json& payload, std::int64_t occurred_at_ms,
                                                    const std::optional<std::string>& dedupe_key)
{
  constexpr char sql[] =
      "INSERT INTO life_records (user_id, domain, payload, occurred_at_ms, dedupe_key) VALUES (?, ?, ?, ?, ?) "
      "ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  const std::string serialized = payload.dump();
  const std::string dedupe_value = dedupe_key.value_or("");
  my_bool dedupe_is_null = !dedupe_key.has_value();
  std::array<unsigned long, 3> lengths{};
  std::array<MYSQL_BIND, 5> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  db::bindString(parameters[1], domain, lengths[0]);
  db::bindString(parameters[2], serialized, lengths[1]);
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &occurred_at_ms;
  db::bindString(parameters[4], dedupe_value, lengths[2]);
  parameters[4].is_null = &dedupe_is_null;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot create life record");
  }
  const bool created = mysql_stmt_affected_rows(statement.get()) == 1;
  const std::uint64_t record_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(statement.get()));
  const std::optional<LifeRecord> record = findOwned(user_id, record_id);
  if (!record.has_value()) {
    throw db::DatabaseError("created life record is missing");
  }
  return {created, record.value()};
}

std::vector<LifeRecord> LifeRecordRepository::listOwned(std::uint64_t user_id, const std::string& domain)
{
  constexpr char sql[] =
      "SELECT id, user_id, domain, payload, occurred_at_ms FROM life_records "
      "WHERE user_id = ? AND domain = ? ORDER BY occurred_at_ms DESC, id DESC";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  unsigned long domain_length = 0;
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  db::bindString(parameters[1], domain, domain_length);
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot list life records");
  }
  Result result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind life record list");
  }
  std::vector<LifeRecord> records;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0) {
      throw db::DatabaseError("cannot fetch life record list");
    }
    records.push_back(result.record());
  }
  return records;
}

std::optional<LifeRecord> LifeRecordRepository::findOwned(std::uint64_t user_id, std::uint64_t record_id)
{
  constexpr char sql[] =
      "SELECT id, user_id, domain, payload, occurred_at_ms FROM life_records WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &record_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot query owned life record");
  }
  Result result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind owned life record");
  }
  const int fetched = mysql_stmt_fetch(statement.get());
  if (fetched == MYSQL_NO_DATA) {
    return std::nullopt;
  }
  if (fetched != 0) {
    throw db::DatabaseError("cannot fetch owned life record");
  }
  return result.record();
}

bool LifeRecordRepository::updateOwned(std::uint64_t user_id, std::uint64_t record_id,
                                       const nlohmann::json& payload, std::int64_t occurred_at_ms,
                                       const std::optional<std::string>& dedupe_key)
{
  constexpr char sql[] =
      "UPDATE life_records SET payload = ?, occurred_at_ms = ?, dedupe_key = ? WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  const std::string serialized = payload.dump();
  const std::string dedupe_value = dedupe_key.value_or("");
  my_bool dedupe_is_null = !dedupe_key.has_value();
  unsigned long payload_length = 0;
  unsigned long dedupe_length = 0;
  std::array<MYSQL_BIND, 5> parameters{};
  db::bindString(parameters[0], serialized, payload_length);
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &occurred_at_ms;
  db::bindString(parameters[2], dedupe_value, dedupe_length);
  parameters[2].is_null = &dedupe_is_null;
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &user_id;
  parameters[3].is_unsigned = true;
  parameters[4].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[4].buffer = &record_id;
  parameters[4].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot update life record");
  }
  return mysql_stmt_affected_rows(statement.get()) > 0 || findOwned(user_id, record_id).has_value();
}

bool LifeRecordRepository::deleteOwned(std::uint64_t user_id, std::uint64_t record_id)
{
  constexpr char sql[] = "DELETE FROM life_records WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &record_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot delete life record");
  }
  return mysql_stmt_affected_rows(statement.get()) > 0;
}

}  // namespace qaiservice::life
