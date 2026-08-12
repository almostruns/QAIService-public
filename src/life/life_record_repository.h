#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::life {

struct LifeRecord {
  std::uint64_t id;
  std::uint64_t user_id;
  std::string domain;
  nlohmann::json payload;
  std::int64_t occurred_at_ms;
};

struct CreateLifeRecordResult {
  bool created;
  LifeRecord record;
};

class LifeRecordRepository {
 public:
  explicit LifeRecordRepository(db::MySqlConnection& connection);

  [[nodiscard]] CreateLifeRecordResult create(std::uint64_t user_id, const std::string& domain,
                                              const nlohmann::json& payload, std::int64_t occurred_at_ms,
                                              const std::optional<std::string>& dedupe_key = {});
  [[nodiscard]] std::vector<LifeRecord> listOwned(std::uint64_t user_id, const std::string& domain);
  [[nodiscard]] std::optional<LifeRecord> findOwned(std::uint64_t user_id, std::uint64_t record_id);
  [[nodiscard]] bool updateOwned(std::uint64_t user_id, std::uint64_t record_id, const nlohmann::json& payload,
                                 std::int64_t occurred_at_ms,
                                 const std::optional<std::string>& dedupe_key = std::nullopt);
  [[nodiscard]] bool deleteOwned(std::uint64_t user_id, std::uint64_t record_id);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::life
