#include "web_search/web_search_router.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace qaiservice::web_search {
namespace {

constexpr std::size_t kMaximumQueryBytes = 72;
constexpr std::size_t kMaximumQueries = 3;
constexpr std::size_t kMaximumReasonBytes = 256;
constexpr std::size_t kMaximumRemovedFields = 16;
constexpr std::size_t kMaximumRemovedFieldBytes = 64;

std::size_t queryUnits(const std::string& query)
{
  std::size_t units = 0;
  for (std::size_t index = 0; index < query.size();) {
    const unsigned char first = static_cast<unsigned char>(query[index]);
    std::size_t bytes = 1;
    if ((first & 0xe0U) == 0xc0U) {
      bytes = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
      bytes = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
      bytes = 4;
    }
    if (index + bytes > query.size()) {
      return kMaximumQueryBytes + 1;
    }
    units += bytes == 1 ? 1 : 2;
    index += bytes;
  }
  return units;
}

bool containsAny(const std::string& message, const std::vector<std::string_view>& markers)
{
  return std::any_of(markers.begin(), markers.end(), [&message](std::string_view marker) {
    return message.find(marker) != std::string::npos;
  });
}

bool validStringArray(const nlohmann::json& value, std::size_t maximum_items, std::size_t maximum_bytes)
{
  if (!value.is_array() || value.size() > maximum_items) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [maximum_bytes](const nlohmann::json& item) {
    return item.is_string() && !item.get_ref<const std::string&>().empty() &&
           queryUnits(item.get_ref<const std::string&>()) <= maximum_bytes;
  });
}

bool hasExactPlanShape(const nlohmann::json& value)
{
  static constexpr std::array<std::string_view, 5> keys = {
      "needs_web", "required_for_answer", "queries", "reason", "sensitive_fields_removed"};
  if (!value.is_object() || value.size() != keys.size()) {
    return false;
  }
  return std::all_of(keys.begin(), keys.end(), [&value](std::string_view key) {
    return value.contains(key);
  });
}

std::optional<WebSearchPlan> deterministicPlan(const std::string& message)
{
  const std::vector<std::string_view> required_markers = {
      "今天", "今日", "现在", "当前", "实时", "最新", "价格", "金价", "汇率", "天气", "股价", "新闻"};
  if (!containsAny(message, required_markers)) {
    const std::vector<std::string_view> stable_markers = {
        "解释", "什么是", "原理", "概念", "怎么理解", "如何实现", "代码示例"};
    if (containsAny(message, stable_markers)) {
      WebSearchPlan plan;
      plan.reason = "问题属于稳定知识，可直接回答";
      return plan;
    }
    return std::nullopt;
  }

  WebSearchPlan plan;
  plan.needs_web = true;
  plan.required_for_answer = true;
  plan.reason = "问题包含时效性信息，必须先生成脱敏查询再使用联网证据";
  return plan;
}

}  // namespace

std::optional<WebSearchPlan> parseModelPlan(const std::string& raw_plan)
{
  const auto parsed = nlohmann::json::parse(raw_plan, nullptr, false);
  if (parsed.is_discarded() || !hasExactPlanShape(parsed)) {
    return std::nullopt;
  }
  if (!parsed["needs_web"].is_boolean() || !parsed["required_for_answer"].is_boolean() ||
      !parsed["reason"].is_string()) {
    return std::nullopt;
  }
  if (!validStringArray(parsed["queries"], kMaximumQueries, kMaximumQueryBytes)) {
    return std::nullopt;
  }
  if (!validStringArray(parsed["sensitive_fields_removed"], kMaximumRemovedFields, kMaximumRemovedFieldBytes) &&
      !parsed["sensitive_fields_removed"].empty()) {
    return std::nullopt;
  }

  WebSearchPlan plan;
  plan.needs_web = parsed["needs_web"].get<bool>();
  plan.required_for_answer = parsed["required_for_answer"].get<bool>();
  plan.queries = parsed["queries"].get<std::vector<std::string>>();
  plan.reason = parsed["reason"].get<std::string>();
  plan.sensitive_fields_removed = parsed["sensitive_fields_removed"].get<std::vector<std::string>>();

  if (plan.reason.size() > kMaximumReasonBytes) {
    return std::nullopt;
  }
  if (plan.needs_web && plan.queries.empty()) {
    return std::nullopt;
  }
  if (!plan.needs_web && (plan.required_for_answer || !plan.queries.empty())) {
    return std::nullopt;
  }
  return plan;
}

std::optional<WebSearchPlan> WebSearchRouter::deterministicRoute(const std::string& message) const
{
  return deterministicPlan(message);
}

WebSearchPlan WebSearchRouter::route(const std::string& message,
                                     const std::optional<WebSearchPlan>& model_plan) const
{
  const auto deterministic = deterministicPlan(message);
  if (deterministic.has_value()) {
    WebSearchPlan routed = deterministic.value();
    if (routed.required_for_answer && model_plan.has_value() && model_plan->needs_web) {
      routed.queries = model_plan->queries;
      routed.sensitive_fields_removed = model_plan->sensitive_fields_removed;
    }
    return routed;
  }
  if (model_plan.has_value()) {
    return model_plan.value();
  }
  return WebSearchPlan{};
}

}  // namespace qaiservice::web_search
