#include "life/life_routes.h"

#include "db/database_worker.h"
#include "http/json_response.h"
#include "http/router.h"
#include "life/activity_insights.h"
#include "life/financial_insights.h"
#include "life/life_record_repository.h"
#include "life/life_validation.h"
#include "users/auth_middleware.h"
#include "util/time.h"

#include "logging/log.h"
#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace qaiservice::life {
namespace {

bool supportedDomain(const std::string& domain)
{
  static const std::unordered_set<std::string> domains{
      "calendar", "tasks", "focus", "habits", "checkins", "ledger", "accounts", "budget_settings",
      "monthly_budgets", "weight", "sleep"};
  return domains.find(domain) != domains.end();
}

std::optional<std::string> pathValue(const http::Request& request, const std::string& name)
{
  const auto value = request.path_parameters.find(name);
  return value == request.path_parameters.end() ? std::nullopt : std::optional<std::string>(value->second);
}

std::optional<std::uint64_t> recordId(const http::Request& request)
{
  const std::optional<std::string> value = pathValue(request, "record_id");
  if (!value.has_value()) {
    return std::nullopt;
  }
  std::uint64_t id = 0;
  const auto parsed = std::from_chars(value->data(), value->data() + value->size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || id == 0) {
    return std::nullopt;
  }
  return id;
}

bool projectSupports(const LifeRecord& project, const std::string& project_type)
{
  if (project.domain != "habits" || project.payload.value("archived", false)) {
    return false;
  }
  if (!project.payload.contains("project_type")) {
    return true;
  }
  return project.payload.value("project_type", "") == project_type;
}

bool ledgerAccountIsValid(LifeRecordRepository& repository, std::uint64_t user_id,
                          const nlohmann::json& payload)
{
  if (!payload.contains("account_id")) {
    return true;
  }
  const std::uint64_t account_id = payload["account_id"].get<std::uint64_t>();
  const std::optional<LifeRecord> account = repository.findOwned(user_id, account_id);
  if (!account.has_value() || account->domain != "accounts" || account->payload.value("archived", false)) {
    return false;
  }
  return account->payload.value("currency", "") == payload.value("currency", "");
}

std::optional<std::string> monthFromQuery(const std::string& query)
{
  constexpr char prefix[] = "month=";
  if (query.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const std::string month = query.substr(sizeof(prefix) - 1);
  if (month.size() != 7) {
    return std::nullopt;
  }
  return month;
}

nlohmann::json recordJson(const LifeRecord& record)
{
  return {{"id", record.id}, {"domain", record.domain}, {"occurred_at_ms", record.occurred_at_ms},
          {"data", record.payload}};
}

void databaseBusy(const http::ResponseWriter& writer)
{
  writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
}

}  // namespace

void registerLifeRoutes(http::Router& router, db::DatabaseWorker& database_worker)
{
  router.add("POST", "/api/life/activity/insights",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    std::optional<ActivityRange> range;
    try {
      range = parseActivityRange(nlohmann::json::parse(request.body));
    } catch (const nlohmann::json::exception&) {
    }
    if (!range.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_activity_range"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), range = range.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        const std::vector<LifeRecord> projects = repository.listOwned(user_id, "habits");
        const std::vector<LifeRecord> focus = repository.listOwned(user_id, "focus");
        const std::vector<LifeRecord> checkins = repository.listOwned(user_id, "checkins");
        nlohmann::json insights = buildActivityInsights(projects, focus, checkins, range.start_ms, range.end_ms);
        QAI_LOG(info, qaiservice::log::Module::kLife) << "activity_insights_ready user_id=" << user_id
                                                       << " project_count=" << projects.size()
                                                       << " focus_count=" << focus.size()
                                                       << " checkin_count=" << checkins.size();
        writer.send(http::jsonResponse(200, std::move(insights)));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_activity_insights user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "activity_insights_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("GET", "/api/life/finance/insights",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> month = monthFromQuery(request.query);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!month.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_financial_month"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), month = month.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        const std::vector<LifeRecord> accounts = repository.listOwned(user_id, "accounts");
        const std::vector<LifeRecord> ledger = repository.listOwned(user_id, "ledger");
        const std::vector<LifeRecord> settings = repository.listOwned(user_id, "budget_settings");
        const std::vector<LifeRecord> monthly = repository.listOwned(user_id, "monthly_budgets");
        nlohmann::json insights = buildFinancialInsights(
            accounts, ledger, settings, monthly, month, util::currentTimeMilliseconds());
        QAI_LOG(info, qaiservice::log::Module::kLife) << "financial_insights_ready user_id=" << user_id
                                                       << " account_count=" << accounts.size()
                                                       << " ledger_count=" << ledger.size();
        writer.send(http::jsonResponse(200, std::move(insights)));
      } catch (const std::invalid_argument&) {
        writer.send(http::jsonResponse(400, {{"error", "invalid_financial_month"}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "financial_insights user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "financial_insights_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("GET", "/api/life/{domain}", [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> domain = pathValue(request, "domain");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!domain.has_value() || !supportedDomain(domain.value())) {
      writer.send(http::jsonResponse(404, {{"error", "life_domain_not_found"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), domain = domain.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        nlohmann::json records = nlohmann::json::array();
        for (const LifeRecord& record : repository.listOwned(user_id, domain)) {
          records.push_back(recordJson(record));
        }
        QAI_LOG(info, qaiservice::log::Module::kLife) << "life_records_listed user_id=" << user_id
                                                       << " domain=" << domain
                                                       << " count=" << records.size();
        writer.send(http::jsonResponse(200, {{"records", std::move(records)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_domain_list user_id=" << user_id << " domain=" << domain << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "life_domain_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("POST", "/api/life/{domain}", [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> domain = pathValue(request, "domain");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!domain.has_value() || !supportedDomain(domain.value())) {
      writer.send(http::jsonResponse(404, {{"error", "life_domain_not_found"}}));
      return;
    }
    LifeValidationResult validation;
    try {
      validation = validateLifeRecord(domain.value(), nlohmann::json::parse(request.body));
    } catch (const nlohmann::json::exception&) {
      validation.error = "invalid_json";
    }
    if (!validation.valid()) {
      writer.send(http::jsonResponse(400, {{"error", validation.error}}));
      return;
    }
    const std::int64_t occurred_at = lifeRecordOccurredAt(domain.value(), validation.normalized, util::currentTimeMilliseconds());
    const std::optional<std::string> dedupe = lifeRecordDedupeKey(domain.value(), validation.normalized);
    db::DatabaseWorker::Task task = [user_id = user_id.value(), domain = domain.value(),
                                     payload = std::move(validation.normalized), occurred_at, dedupe, writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        if (domain == "ledger" && !ledgerAccountIsValid(repository, user_id, payload)) {
          writer.send(http::jsonResponse(400, {{"error", "invalid_account"}}));
          return;
        }
        if (domain == "focus" && payload.contains("project_id")) {
          const std::uint64_t project_id = payload["project_id"].get<std::uint64_t>();
          const std::optional<LifeRecord> project = repository.findOwned(user_id, project_id);
          if (!project.has_value() || !projectSupports(project.value(), "focus")) {
            writer.send(http::jsonResponse(400, {{"error", "invalid_activity_project"}}));
            return;
          }
        }
        if (domain == "checkins" && payload.contains("habit_id")) {
          const std::uint64_t project_id = payload["habit_id"].get<std::uint64_t>();
          const std::optional<LifeRecord> project = repository.findOwned(user_id, project_id);
          if (!project.has_value() || !projectSupports(project.value(), "checkin")) {
            writer.send(http::jsonResponse(400, {{"error", "invalid_activity_project"}}));
            return;
          }
        }
        if (domain == "focus") {
          for (const LifeRecord& existing : repository.listOwned(user_id, domain)) {
            if (existing.payload.value("status", "active") == "active") {
              writer.send(http::jsonResponse(409, {{"error", "focus_session_already_active"}}));
              return;
            }
          }
        }
        const CreateLifeRecordResult result = repository.create(user_id, domain, payload, occurred_at, dedupe);
        if (!result.created) {
          QAI_LOG(info, qaiservice::log::Module::kLife) << "life_record_deduplicated user_id=" << user_id
                                                         << " domain=" << domain
                                                         << " record_id=" << result.record.id;
          writer.send(http::jsonResponse(200, {{"duplicate", true}, {"record", recordJson(result.record)}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kLife) << "life_record_created user_id=" << user_id
                                                       << " domain=" << domain
                                                       << " record_id=" << result.record.id;
        writer.send(http::jsonResponse(201, {{"duplicate", false}, {"record", recordJson(result.record)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_record_create user_id=" << user_id << " domain=" << domain << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "life_record_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("PATCH", "/api/life/{domain}/{record_id}",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> domain = pathValue(request, "domain");
    const std::optional<std::uint64_t> id = recordId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!domain.has_value() || !supportedDomain(domain.value()) || !id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_life_record"}}));
      return;
    }
    LifeValidationResult validation;
    try {
      validation = validateLifeRecord(domain.value(), nlohmann::json::parse(request.body));
    } catch (const nlohmann::json::exception&) {
      validation.error = "invalid_json";
    }
    if (!validation.valid()) {
      writer.send(http::jsonResponse(400, {{"error", validation.error}}));
      return;
    }
    const std::int64_t occurred_at = lifeRecordOccurredAt(domain.value(), validation.normalized, util::currentTimeMilliseconds());
    db::DatabaseWorker::Task task = [user_id = user_id.value(), domain = domain.value(), id = id.value(),
                                     payload = std::move(validation.normalized), occurred_at, writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        const std::optional<LifeRecord> existing = repository.findOwned(user_id, id);
        if (!existing.has_value() || existing->domain != domain) {
          writer.send(http::jsonResponse(404, {{"error", "life_record_not_found"}}));
          return;
        }
        if (domain == "ledger" && !ledgerAccountIsValid(repository, user_id, payload)) {
          writer.send(http::jsonResponse(400, {{"error", "invalid_account"}}));
          return;
        }
        const std::optional<std::string> new_dedupe = lifeRecordDedupeKey(domain, payload);
        if (new_dedupe.has_value() && lifeRecordDedupeKey(domain, existing->payload) != new_dedupe) {
          for (const LifeRecord& record : repository.listOwned(user_id, domain)) {
            if (record.id != id && lifeRecordDedupeKey(domain, record.payload) == new_dedupe) {
              writer.send(http::jsonResponse(409, {{"error", "duplicate_life_record"}}));
              return;
            }
          }
        }
        if (!repository.updateOwned(user_id, id, payload, occurred_at, new_dedupe)) {
          writer.send(http::jsonResponse(404, {{"error", "life_record_not_found"}}));
          return;
        }
        LifeRecord updated = existing.value();
        updated.payload = payload;
        updated.occurred_at_ms = occurred_at;
        QAI_LOG(info, qaiservice::log::Module::kLife) << "life_record_updated user_id=" << user_id
                                                       << " domain=" << domain
                                                       << " record_id=" << id;
        writer.send(http::jsonResponse(200, {{"record", recordJson(updated)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_record_update user_id=" << user_id << " domain=" << domain << " id=" << id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "life_record_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("DELETE", "/api/life/{domain}/{record_id}",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> domain = pathValue(request, "domain");
    const std::optional<std::uint64_t> id = recordId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!domain.has_value() || !supportedDomain(domain.value()) || !id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_life_record"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), domain = domain.value(), id = id.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        const std::optional<LifeRecord> existing = repository.findOwned(user_id, id);
        if (!existing.has_value() || existing->domain != domain || !repository.deleteOwned(user_id, id)) {
          writer.send(http::jsonResponse(404, {{"error", "life_record_not_found"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kLife) << "life_record_deleted user_id=" << user_id
                                                       << " domain=" << domain
                                                       << " record_id=" << id;
        writer.send({204, "application/json", ""});
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_record_delete user_id=" << user_id << " domain=" << domain << " id=" << id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "life_record_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });

  router.add("GET", "/api/life/summary", [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), writer](db::MySqlConnection& connection) {
      try {
        LifeRecordRepository repository(connection);
        std::int64_t income = 0;
        std::int64_t expense = 0;
        for (const LifeRecord& record : repository.listOwned(user_id, "ledger")) {
          const std::int64_t amount = record.payload.value("amount_minor", 0LL);
          if (record.payload.value("direction", "") == "income") {
            income += amount;
          } else {
            expense += amount;
          }
        }
        const std::vector<LifeRecord> weights = repository.listOwned(user_id, "weight");
        const std::vector<LifeRecord> sleeps = repository.listOwned(user_id, "sleep");
        std::int64_t sleep_minutes = 0;
        for (const LifeRecord& record : sleeps) {
          sleep_minutes += record.payload.value("duration_minutes", 0LL);
        }
        QAI_LOG(info, qaiservice::log::Module::kLife) << "life_summary_ready user_id=" << user_id
                                                       << " weight_count=" << weights.size()
                                                       << " sleep_count=" << sleeps.size();
        writer.send(http::jsonResponse(200, {{"ledger", {{"income_minor", income}, {"expense_minor", expense}}},
                                       {"latest_weight_grams", weights.empty() ? 0 : weights.front().payload.value("grams", 0)},
                                       {"average_sleep_minutes", sleeps.empty() ? 0 : sleep_minutes / static_cast<std::int64_t>(sleeps.size())}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kLife) << "life_summary user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "life_summary_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      databaseBusy(writer);
    }
  });
}

}  // namespace qaiservice::life
