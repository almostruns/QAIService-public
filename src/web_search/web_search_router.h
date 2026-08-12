#pragma once

#include "web_search/web_search_types.h"

#include <optional>
#include <string>

namespace qaiservice::web_search {

std::optional<WebSearchPlan> parseModelPlan(const std::string& raw_plan);

class WebSearchRouter {
public:
  [[nodiscard]] std::optional<WebSearchPlan> deterministicRoute(const std::string& message) const;
  WebSearchPlan route(const std::string& message, const std::optional<WebSearchPlan>& model_plan) const;
};

}  // namespace qaiservice::web_search
