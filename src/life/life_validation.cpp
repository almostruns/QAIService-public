#include "life/life_validation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace qaiservice::life {
namespace {

bool stringField(const nlohmann::json& input, const char* name, std::size_t maximum = 255)
{
  return input.contains(name) && input[name].is_string() && !input[name].get_ref<const std::string&>().empty() &&
         input[name].get_ref<const std::string&>().size() <= maximum;
}

bool integerField(const nlohmann::json& input, const char* name)
{
  return input.contains(name) && input[name].is_number_integer();
}

bool validActivityColor(const std::string& color)
{
  if (color.size() != 7 || color.front() != '#') {
    return false;
  }
  return std::all_of(color.begin() + 1, color.end(), [](unsigned char value) {
    return std::isxdigit(value) != 0;
  });
}

bool validProjectIcon(const std::string& icon)
{
  if (icon.empty() || icon.size() > 16) {
    return false;
  }
  return std::all_of(icon.begin(), icon.end(), [](unsigned char value) {
    return value >= 32 && value != 127;
  });
}

bool validCurrency(const std::string& currency)
{
  return currency.size() == 3 && std::all_of(currency.begin(), currency.end(), [](unsigned char value) {
    return std::isupper(value) != 0;
  });
}

bool validMonth(const std::string& month)
{
  if (month.size() != 7 || month[4] != '-') {
    return false;
  }
  if (!std::all_of(month.begin(), month.begin() + 4, [](unsigned char value) {
        return std::isdigit(value) != 0;
      }) || !std::all_of(month.begin() + 5, month.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return false;
  }
  const int month_number = (month[5] - '0') * 10 + (month[6] - '0');
  return month_number >= 1 && month_number <= 12;
}

std::string asciiLowercase(const std::string& value)
{
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
    return character < 128 ? static_cast<char>(std::tolower(character)) : static_cast<char>(character);
  });
  return normalized;
}

std::string trimWhitespace(const std::string& value)
{
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

LifeValidationResult invalid(std::string error)
{
  return {nlohmann::json::object(), std::move(error)};
}

}  // namespace

bool LifeValidationResult::valid() const
{
  return error.empty();
}

LifeValidationResult validateLifeRecord(const std::string& domain, const nlohmann::json& input)
{
  if (!input.is_object()) {
    return invalid("invalid_record");
  }
  if (domain == "calendar") {
    if (!stringField(input, "title") || !integerField(input, "starts_at_ms") || !integerField(input, "ends_at_ms") ||
        !stringField(input, "timezone", 64)) {
      return invalid("invalid_calendar_record");
    }
    if (input["ends_at_ms"].get<std::int64_t>() <= input["starts_at_ms"].get<std::int64_t>()) {
      return invalid("invalid_time_range");
    }
    return {input, ""};
  }
  if (domain == "tasks") {
    if (!stringField(input, "title") || !integerField(input, "due_at_ms")) {
      return invalid("invalid_task");
    }
    nlohmann::json normalized = input;
    normalized["completed"] = input.value("completed", false);
    return {std::move(normalized), ""};
  }
  if (domain == "focus") {
    if (!integerField(input, "started_at_ms") || !integerField(input, "planned_minutes")) {
      return invalid("invalid_focus_record");
    }
    const std::string timer_mode = input.value("timer_mode", "countdown");
    if (timer_mode != "stopwatch" && timer_mode != "countdown") {
      return invalid("invalid_timer_mode");
    }
    const std::int64_t minutes = input["planned_minutes"].get<std::int64_t>();
    const bool valid_stopwatch_duration = timer_mode == "stopwatch" && minutes == 0;
    const bool valid_countdown_duration = timer_mode == "countdown" && minutes >= 1 && minutes <= 240;
    if (!valid_stopwatch_duration && !valid_countdown_duration) {
      return invalid("invalid_focus_duration");
    }
    if (input.contains("project_id") &&
        (!integerField(input, "project_id") || input["project_id"].get<std::int64_t>() <= 0)) {
      return invalid("invalid_activity_project");
    }
    nlohmann::json normalized = input;
    normalized["timer_mode"] = timer_mode;
    normalized["status"] = input.value("status", "active");
    if (input.contains("paused_seconds") &&
        (!integerField(input, "paused_seconds") || input["paused_seconds"].get<std::int64_t>() < 0)) {
      return invalid("invalid_focus_pause");
    }
    const std::int64_t paused_seconds = input.value("paused_seconds", 0LL);
    normalized["paused_seconds"] = paused_seconds;

    const bool has_break_start = input.contains("break_started_at_ms");
    const bool has_break_end = input.contains("break_until_ms");
    if (has_break_start != has_break_end) {
      return invalid("invalid_focus_break");
    }
    std::optional<std::int64_t> break_started_at_ms;
    std::optional<std::int64_t> break_until_ms;
    if (has_break_start) {
      if (!integerField(input, "break_started_at_ms") || !integerField(input, "break_until_ms")) {
        return invalid("invalid_focus_break");
      }
      break_started_at_ms = input["break_started_at_ms"].get<std::int64_t>();
      break_until_ms = input["break_until_ms"].get<std::int64_t>();
      if (break_started_at_ms.value() < input["started_at_ms"].get<std::int64_t>() ||
          break_until_ms.value() <= break_started_at_ms.value()) {
        return invalid("invalid_focus_break");
      }
      const std::uint64_t break_duration_ms = static_cast<std::uint64_t>(break_until_ms.value()) -
                                              static_cast<std::uint64_t>(break_started_at_ms.value());
      if (break_duration_ms > 300000) {
        return invalid("invalid_focus_break");
      }
    }
    if (normalized["status"] != "active" && normalized["status"] != "completed" &&
        normalized["status"] != "cancelled") {
      return invalid("invalid_focus_status");
    }
    if (normalized["status"] != "active") {
      if (!integerField(input, "ended_at_ms") ||
          input["ended_at_ms"].get<std::int64_t>() < input["started_at_ms"].get<std::int64_t>()) {
        return invalid("invalid_focus_end");
      }
      const std::int64_t ended_at_ms = input["ended_at_ms"].get<std::int64_t>();
      const std::int64_t started_at_ms = input["started_at_ms"].get<std::int64_t>();
      const std::uint64_t wall_milliseconds = static_cast<std::uint64_t>(ended_at_ms) -
                                              static_cast<std::uint64_t>(started_at_ms);
      const std::uint64_t wall_seconds = wall_milliseconds / 1000;
      std::uint64_t current_break_seconds = 0;
      if (break_started_at_ms.has_value()) {
        if (break_started_at_ms.value() > ended_at_ms) {
          return invalid("invalid_focus_break");
        }
        const std::int64_t effective_break_end = std::min(ended_at_ms, break_until_ms.value());
        const std::uint64_t current_break_milliseconds = static_cast<std::uint64_t>(effective_break_end) -
                                                         static_cast<std::uint64_t>(break_started_at_ms.value());
        current_break_seconds = current_break_milliseconds / 1000;
      }
      const std::uint64_t accumulated_pause_seconds = static_cast<std::uint64_t>(paused_seconds);
      const bool accumulated_pause_exceeds_session = accumulated_pause_seconds > wall_seconds;
      const bool current_break_exceeds_session = !accumulated_pause_exceeds_session &&
                                                 current_break_seconds > wall_seconds - accumulated_pause_seconds;
      if (accumulated_pause_exceeds_session || current_break_exceeds_session) {
        return invalid("invalid_focus_pause");
      }
      normalized["elapsed_seconds"] = static_cast<std::int64_t>(wall_seconds - accumulated_pause_seconds -
                                                                 current_break_seconds);
    }
    return {std::move(normalized), ""};
  }
  if (domain == "habits") {
    if (!stringField(input, "name", 100) || !stringField(input, "project_type", 16)) {
      return invalid("invalid_habit");
    }
    const std::string project_type = input["project_type"].get<std::string>();
    if (project_type != "checkin" && project_type != "focus") {
      return invalid("invalid_project_type");
    }
    const bool invalid_note = input.contains("note") &&
                              (!input["note"].is_string() ||
                               input["note"].get_ref<const std::string&>().size() > 500);
    if (invalid_note) {
      return invalid("invalid_project_note");
    }
    const bool invalid_icon_type = input.contains("icon") && !input["icon"].is_string();
    if (invalid_icon_type) {
      return invalid("invalid_project_icon");
    }
    const std::string icon = input.value("icon", "●");
    if (!validProjectIcon(icon)) {
      return invalid("invalid_project_icon");
    }
    const std::string name = trimWhitespace(input["name"].get<std::string>());
    if (name.empty()) {
      return invalid("invalid_habit");
    }
    nlohmann::json normalized = input;
    normalized["name"] = name;
    normalized["project_type"] = project_type;
    normalized["note"] = input.value("note", "");
    normalized["icon"] = icon;
    normalized["color"] = input.value("color", "#5E7C6B");
    normalized["archived"] = input.value("archived", false);
    if (!normalized["color"].is_string() ||
        !validActivityColor(normalized["color"].get<std::string>())) {
      return invalid("invalid_activity_color");
    }
    if (!normalized["archived"].is_boolean()) {
      return invalid("invalid_habit");
    }
    return {std::move(normalized), ""};
  }
  if (domain == "checkins") {
    if (!integerField(input, "habit_id") || !stringField(input, "local_date", 10)) {
      return invalid("invalid_checkin");
    }
    return {input, ""};
  }
  if (domain == "ledger") {
    if (!stringField(input, "direction", 7) || !integerField(input, "amount_minor") ||
        !stringField(input, "currency", 3) || !stringField(input, "category", 100) ||
        !integerField(input, "occurred_at_ms")) {
      return invalid(integerField(input, "amount_minor") ? "invalid_ledger_record" : "invalid_amount");
    }
    const std::string direction = input["direction"].get<std::string>();
    const std::string currency = input["currency"].get<std::string>();
    const std::int64_t amount = input["amount_minor"].get<std::int64_t>();
    if ((direction != "income" && direction != "expense") || amount <= 0 || !validCurrency(currency)) {
      return invalid("invalid_ledger_record");
    }
    if (input.contains("account_id") &&
        (!integerField(input, "account_id") || input["account_id"].get<std::int64_t>() <= 0)) {
      return invalid("invalid_account");
    }
    return {input, ""};
  }
  if (domain == "accounts") {
    if (!stringField(input, "name", 100) || !stringField(input, "currency", 3) ||
        !integerField(input, "opening_balance_minor")) {
      return invalid("invalid_account");
    }
    const std::string name = trimWhitespace(input["name"].get<std::string>());
    const std::string currency = input["currency"].get<std::string>();
    nlohmann::json normalized = input;
    normalized["name"] = name;
    normalized["currency"] = currency;
    normalized["color"] = input.value("color", "#5E7C6B");
    normalized["archived"] = input.value("archived", false);
    if (name.empty() || !validCurrency(currency) || !normalized["color"].is_string() ||
        !validActivityColor(normalized["color"].get<std::string>()) || !normalized["archived"].is_boolean()) {
      return invalid("invalid_account");
    }
    return {std::move(normalized), ""};
  }
  if (domain == "budget_settings") {
    if (!integerField(input, "amount_minor") || !stringField(input, "currency", 3) ||
        input["amount_minor"].get<std::int64_t>() < 0 || !validCurrency(input["currency"].get<std::string>())) {
      return invalid("invalid_budget_setting");
    }
    return {input, ""};
  }
  if (domain == "monthly_budgets") {
    if (!stringField(input, "month", 7) || !integerField(input, "amount_minor") ||
        !stringField(input, "currency", 3) || !validMonth(input["month"].get<std::string>()) ||
        input["amount_minor"].get<std::int64_t>() < 0 || !validCurrency(input["currency"].get<std::string>())) {
      return invalid("invalid_monthly_budget");
    }
    return {input, ""};
  }
  if (domain == "weight") {
    if (!integerField(input, "grams") || !integerField(input, "recorded_at_ms")) {
      return invalid("invalid_weight");
    }
    const std::int64_t grams = input["grams"].get<std::int64_t>();
    if (grams < 25000 || grams > 400000) {
      return invalid("invalid_weight");
    }
    return {input, ""};
  }
  if (domain == "sleep") {
    if (!integerField(input, "started_at_ms") || !integerField(input, "ended_at_ms") ||
        !integerField(input, "quality")) {
      return invalid("invalid_sleep_record");
    }
    const std::int64_t started = input["started_at_ms"].get<std::int64_t>();
    const std::int64_t ended = input["ended_at_ms"].get<std::int64_t>();
    const std::int64_t quality = input["quality"].get<std::int64_t>();
    if (ended <= started || ended - started > 24LL * 60 * 60 * 1000) {
      return invalid("invalid_sleep_range");
    }
    if (quality < 1 || quality > 5) {
      return invalid("invalid_sleep_quality");
    }
    nlohmann::json normalized = input;
    normalized["duration_minutes"] = (ended - started) / 60000;
    return {std::move(normalized), ""};
  }
  return invalid("unsupported_life_domain");
}

std::optional<std::string> lifeRecordDedupeKey(const std::string& domain, const nlohmann::json& payload)
{
  if (domain == "habits") {
    if (!payload.contains("project_type") || !payload["project_type"].is_string() ||
        !payload.contains("name") || !payload["name"].is_string()) {
      return std::nullopt;
    }
    return "project:" + payload["project_type"].get<std::string>() + ":" +
           asciiLowercase(payload["name"].get<std::string>());
  }
  if (domain == "checkins") {
    if (!payload.contains("habit_id") || !payload["habit_id"].is_number_unsigned() ||
        !payload.contains("local_date") || !payload["local_date"].is_string()) {
      return std::nullopt;
    }
    return std::to_string(payload["habit_id"].get<std::uint64_t>()) + ":" +
           payload["local_date"].get<std::string>();
  }
  if (domain == "accounts") {
    if (!payload.contains("name") || !payload["name"].is_string()) {
      return std::nullopt;
    }
    return "account:" + asciiLowercase(payload["name"].get<std::string>());
  }
  if (domain == "budget_settings") {
    return "budget:default";
  }
  if (domain == "monthly_budgets") {
    if (!payload.contains("month") || !payload["month"].is_string()) {
      return std::nullopt;
    }
    return "budget:" + payload["month"].get<std::string>();
  }
  if (payload.contains("client_id") && payload["client_id"].is_string()) {
    return payload["client_id"].get<std::string>();
  }
  return std::nullopt;
}

std::int64_t lifeRecordOccurredAt(const std::string& domain, const nlohmann::json& payload,
                                  std::int64_t fallback_ms)
{
  if (domain == "calendar") {
    return payload["starts_at_ms"].get<std::int64_t>();
  }
  if (domain == "tasks") {
    return payload["due_at_ms"].get<std::int64_t>();
  }
  if (domain == "focus") {
    return payload["started_at_ms"].get<std::int64_t>();
  }
  if (domain == "ledger") {
    return payload["occurred_at_ms"].get<std::int64_t>();
  }
  if (domain == "weight") {
    return payload["recorded_at_ms"].get<std::int64_t>();
  }
  if (domain == "sleep") {
    return payload["started_at_ms"].get<std::int64_t>();
  }
  return fallback_ms;
}

}  // namespace qaiservice::life
