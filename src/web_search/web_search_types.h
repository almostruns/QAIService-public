#pragma once

#include <string>
#include <vector>

namespace qaiservice::web_search {

struct WebSearchPlan {
  bool needs_web{false};
  bool required_for_answer{false};
  std::vector<std::string> queries;
  std::string reason;
  std::vector<std::string> sensitive_fields_removed;
};

struct QueryPrivacyResult {
  bool safe{true};
  std::vector<std::string> risk_types;
};

}  // namespace qaiservice::web_search
