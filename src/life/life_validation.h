#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace qaiservice::life {

struct LifeValidationResult {
  nlohmann::json normalized;
  std::string error;

  [[nodiscard]] bool valid() const;
};

[[nodiscard]] LifeValidationResult validateLifeRecord(const std::string& domain, const nlohmann::json& input);
[[nodiscard]] std::optional<std::string> lifeRecordDedupeKey(const std::string& domain,
                                                             const nlohmann::json& payload);
[[nodiscard]] std::int64_t lifeRecordOccurredAt(const std::string& domain, const nlohmann::json& payload,
                                                std::int64_t fallback_ms);

}  // namespace qaiservice::life
