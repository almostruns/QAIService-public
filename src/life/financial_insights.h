#pragma once

#include "life/life_record_repository.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace qaiservice::life {

[[nodiscard]] nlohmann::json buildFinancialInsights(
    const std::vector<LifeRecord>& accounts, const std::vector<LifeRecord>& ledger,
    const std::vector<LifeRecord>& budget_settings, const std::vector<LifeRecord>& monthly_budgets,
    const std::string& month, std::int64_t now_ms);

}  // namespace qaiservice::life
