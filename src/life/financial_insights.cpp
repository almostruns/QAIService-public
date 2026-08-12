#include "life/financial_insights.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qaiservice::life {
namespace {

struct MonthPeriod {
  std::int64_t start_ms;
  std::int64_t end_ms;
  std::int64_t elapsed_days;
  std::int64_t remaining_days;
};

bool parseMonth(const std::string& month, int& year, int& month_number)
{
  if (month.size() != 7 || month[4] != '-') {
    return false;
  }
  try {
    year = std::stoi(month.substr(0, 4));
    month_number = std::stoi(month.substr(5, 2));
  } catch (const std::exception&) {
    return false;
  }
  return year >= 1970 && year <= 9999 && month_number >= 1 && month_number <= 12;
}

std::int64_t toMilliseconds(std::tm value)
{
  value.tm_isdst = -1;
  const std::time_t seconds = std::mktime(&value);
  if (seconds == static_cast<std::time_t>(-1)) {
    throw std::invalid_argument("invalid financial month");
  }
  return static_cast<std::int64_t>(seconds) * 1000;
}

MonthPeriod monthPeriod(const std::string& month, std::int64_t now_ms)
{
  int year = 0;
  int month_number = 0;
  if (!parseMonth(month, year, month_number)) {
    throw std::invalid_argument("invalid financial month");
  }

  std::tm start{};
  start.tm_year = year - 1900;
  start.tm_mon = month_number - 1;
  start.tm_mday = 1;
  std::tm next = start;
  ++next.tm_mon;
  const std::int64_t start_ms = toMilliseconds(start);
  const std::int64_t end_ms = toMilliseconds(next);
  const std::int64_t days_in_month = (end_ms - start_ms) / (24LL * 60 * 60 * 1000);

  if (now_ms < start_ms) {
    return {start_ms, end_ms, 0, days_in_month};
  }
  if (now_ms >= end_ms) {
    return {start_ms, end_ms, days_in_month, 0};
  }

  const std::time_t now_seconds = static_cast<std::time_t>(now_ms / 1000);
  std::tm local_now{};
  localtime_r(&now_seconds, &local_now);
  const std::int64_t elapsed_days = local_now.tm_mday;
  return {start_ms, end_ms, elapsed_days, days_in_month - elapsed_days + 1};
}

bool inPeriod(const LifeRecord& record, const MonthPeriod& period)
{
  return record.occurred_at_ms >= period.start_ms && record.occurred_at_ms < period.end_ms;
}

std::optional<std::uint64_t> accountId(const LifeRecord& record)
{
  if (!record.payload.contains("account_id") || !record.payload["account_id"].is_number_integer()) {
    return std::nullopt;
  }
  const std::int64_t value = record.payload["account_id"].get<std::int64_t>();
  if (value <= 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value);
}

}  // namespace

nlohmann::json buildFinancialInsights(const std::vector<LifeRecord>& accounts,
                                      const std::vector<LifeRecord>& ledger,
                                      const std::vector<LifeRecord>& budget_settings,
                                      const std::vector<LifeRecord>& monthly_budgets,
                                      const std::string& month, std::int64_t now_ms)
{
  const MonthPeriod period = monthPeriod(month, now_ms);
  std::unordered_map<std::uint64_t, std::int64_t> balances;
  for (const LifeRecord& account : accounts) {
    balances[account.id] = account.payload.value("opening_balance_minor", 0LL);
  }

  std::int64_t month_income = 0;
  std::int64_t month_expense = 0;
  std::size_t unassigned_count = 0;
  std::map<std::string, std::int64_t> category_totals;
  for (const LifeRecord& transaction : ledger) {
    const std::int64_t amount = transaction.payload.value("amount_minor", 0LL);
    const bool income = transaction.payload.value("direction", "") == "income";
    const std::optional<std::uint64_t> linked_account = accountId(transaction);
    if (linked_account.has_value()) {
      const auto balance = balances.find(linked_account.value());
      if (balance != balances.end()) {
        balance->second += income ? amount : -amount;
      }
    } else {
      ++unassigned_count;
    }
    if (!inPeriod(transaction, period)) {
      continue;
    }
    if (income) {
      month_income += amount;
      continue;
    }
    month_expense += amount;
    category_totals[transaction.payload.value("category", "未分类")] += amount;
  }

  nlohmann::json account_rows = nlohmann::json::array();
  std::int64_t total_assets = 0;
  std::int64_t total_liabilities = 0;
  for (const LifeRecord& account : accounts) {
    const std::int64_t balance = balances[account.id];
    if (balance >= 0) {
      total_assets += balance;
    } else {
      total_liabilities += -balance;
    }
    account_rows.push_back({{"id", account.id},
                            {"name", account.payload.value("name", "未命名账户")},
                            {"currency", account.payload.value("currency", "CNY")},
                            {"color", account.payload.value("color", "#5E7C6B")},
                            {"archived", account.payload.value("archived", false)},
                            {"balance_minor", balance}});
  }

  nlohmann::json categories = nlohmann::json::array();
  std::vector<std::pair<std::string, std::int64_t>> ordered_categories(category_totals.begin(),
                                                                       category_totals.end());
  std::sort(ordered_categories.begin(), ordered_categories.end(), [](const auto& left, const auto& right) {
    if (left.second != right.second) {
      return left.second > right.second;
    }
    return left.first < right.first;
  });
  for (const auto& category : ordered_categories) {
    categories.push_back({{"category", category.first}, {"amount_minor", category.second}});
  }

  const bool has_default = !budget_settings.empty();
  const std::int64_t default_budget = has_default ? budget_settings.front().payload.value("amount_minor", 0LL) : 0;
  std::optional<std::int64_t> monthly_override;
  for (const LifeRecord& budget : monthly_budgets) {
    if (budget.payload.value("month", "") == month) {
      monthly_override = budget.payload.value("amount_minor", 0LL);
      break;
    }
  }
  const bool has_budget = monthly_override.has_value() || has_default;
  const std::int64_t effective_budget = monthly_override.value_or(default_budget);
  const std::int64_t remaining_budget = effective_budget - month_expense;
  const std::int64_t average_daily_expense = period.elapsed_days > 0 ? month_expense / period.elapsed_days : 0;
  const std::int64_t remaining_daily_budget = period.remaining_days > 0 ? remaining_budget / period.remaining_days : 0;

  nlohmann::json result{{"month", month},
                        {"has_budget", has_budget},
                        {"has_monthly_override", monthly_override.has_value()},
                        {"default_budget_minor", has_default ? nlohmann::json(default_budget) : nlohmann::json(nullptr)},
                        {"effective_budget_minor", has_budget ? nlohmann::json(effective_budget) : nlohmann::json(nullptr)},
                        {"remaining_budget_minor", has_budget ? nlohmann::json(remaining_budget) : nlohmann::json(nullptr)},
                        {"remaining_daily_budget_minor", has_budget ? nlohmann::json(remaining_daily_budget) : nlohmann::json(nullptr)},
                        {"month_income_minor", month_income},
                        {"month_expense_minor", month_expense},
                        {"average_daily_expense_minor", average_daily_expense},
                        {"elapsed_days", period.elapsed_days},
                        {"remaining_days", period.remaining_days},
                        {"total_assets_minor", total_assets},
                        {"total_liabilities_minor", total_liabilities},
                        {"net_worth_minor", total_assets - total_liabilities},
                        {"unassigned_transaction_count", unassigned_count},
                        {"accounts", std::move(account_rows)},
                        {"categories", std::move(categories)}};
  return result;
}

}  // namespace qaiservice::life
