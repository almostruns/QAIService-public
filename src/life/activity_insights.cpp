#include "life/activity_insights.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace qaiservice::life {
namespace {

constexpr char kDefaultColor[] = "#5E7C6B";
constexpr char kUncategorizedColor[] = "#8B8F8C";

nlohmann::json projectJson(const LifeRecord& project)
{
  return {{"id", project.id},
          {"name", project.payload.value("name", "未命名项目")},
          {"color", project.payload.value("color", kDefaultColor)},
          {"archived", project.payload.value("archived", false)},
          {"elapsed_seconds", 0},
          {"checkins", 0}};
}

}  // namespace

std::optional<ActivityRange> parseActivityRange(const nlohmann::json& input)
{
  constexpr std::int64_t kMaximumRangeMs = 366LL * 24 * 60 * 60 * 1000;
  if (!input.is_object() || !input.contains("range_start_ms") || !input["range_start_ms"].is_number_integer() ||
      !input.contains("range_end_ms") || !input["range_end_ms"].is_number_integer()) {
    return std::nullopt;
  }
  const std::int64_t start_ms = input["range_start_ms"].get<std::int64_t>();
  const std::int64_t end_ms = input["range_end_ms"].get<std::int64_t>();
  if (start_ms < 0 || end_ms <= start_ms || end_ms - start_ms > kMaximumRangeMs) {
    return std::nullopt;
  }
  return ActivityRange{start_ms, end_ms};
}

nlohmann::json buildActivityInsights(const std::vector<LifeRecord>& projects,
                                     const std::vector<LifeRecord>& focus_records,
                                     const std::vector<LifeRecord>& checkins,
                                     std::int64_t range_start_ms,
                                     std::int64_t range_end_ms)
{
  nlohmann::json result{{"total_elapsed_seconds", 0},
                        {"projects", nlohmann::json::array()},
                        {"entries", nlohmann::json::array()}};
  std::unordered_map<std::uint64_t, std::size_t> project_indexes;
  for (const LifeRecord& project : projects) {
    project_indexes.emplace(project.id, result["projects"].size());
    result["projects"].push_back(projectJson(project));
  }

  const auto ensureProject = [&result, &project_indexes](std::uint64_t project_id) {
    const auto existing = project_indexes.find(project_id);
    if (existing != project_indexes.end()) {
      return existing->second;
    }
    const std::size_t index = result["projects"].size();
    result["projects"].push_back({{"id", project_id},
                                  {"name", "未分类"},
                                  {"color", kUncategorizedColor},
                                  {"archived", false},
                                  {"elapsed_seconds", 0},
                                  {"checkins", 0}});
    project_indexes.emplace(project_id, index);
    return index;
  };

  for (const LifeRecord& record : focus_records) {
    if (record.occurred_at_ms < range_start_ms || record.occurred_at_ms >= range_end_ms ||
        record.payload.value("status", "active") != "completed") {
      continue;
    }
    const std::int64_t elapsed_seconds = record.payload.value("elapsed_seconds", 0LL);
    if (elapsed_seconds <= 0) {
      continue;
    }
    const std::uint64_t project_id = record.payload.value("project_id", 0ULL);
    const std::size_t project_index = ensureProject(project_id);
    result["projects"][project_index]["elapsed_seconds"] =
        result["projects"][project_index]["elapsed_seconds"].get<std::int64_t>() + elapsed_seconds;
    result["total_elapsed_seconds"] = result["total_elapsed_seconds"].get<std::int64_t>() + elapsed_seconds;
    result["entries"].push_back({{"id", record.id},
                                  {"project_id", project_id},
                                  {"kind", "focus"},
                                  {"occurred_at_ms", record.occurred_at_ms},
                                  {"elapsed_seconds", elapsed_seconds}});
  }

  for (const LifeRecord& record : checkins) {
    if (record.occurred_at_ms < range_start_ms || record.occurred_at_ms >= range_end_ms) {
      continue;
    }
    const std::uint64_t project_id = record.payload.value("habit_id", 0ULL);
    const std::size_t project_index = ensureProject(project_id);
    result["projects"][project_index]["checkins"] =
        result["projects"][project_index]["checkins"].get<std::int64_t>() + 1;
    result["entries"].push_back({{"id", record.id},
                                  {"project_id", project_id},
                                  {"kind", "checkin"},
                                  {"occurred_at_ms", record.occurred_at_ms},
                                  {"local_date", record.payload.value("local_date", "")}});
  }
  return result;
}

}  // namespace qaiservice::life
