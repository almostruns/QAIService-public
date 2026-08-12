#pragma once

#include "life/life_record_repository.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace qaiservice::life {

struct ActivityRange {
  std::int64_t start_ms;
  std::int64_t end_ms;
};

[[nodiscard]] std::optional<ActivityRange> parseActivityRange(const nlohmann::json& input);

[[nodiscard]] nlohmann::json buildActivityInsights(const std::vector<LifeRecord>& projects,
                                                   const std::vector<LifeRecord>& focus_records,
                                                   const std::vector<LifeRecord>& checkins,
                                                   std::int64_t range_start_ms,
                                                   std::int64_t range_end_ms);

}  // namespace qaiservice::life
