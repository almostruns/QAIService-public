#include "web_search/query_privacy_guard.h"

#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace qaiservice::web_search {
namespace {

using RiskPattern = std::pair<std::string, std::regex>;

const std::vector<RiskPattern>& riskPatterns()
{
  static const std::vector<RiskPattern> patterns = {
      {"phone", std::regex(R"((^|[^0-9])1[3-9][0-9]{9}([^0-9]|$))")},
      {"identity_card", std::regex(R"((^|[^0-9])[0-9]{17}[0-9Xx]([^0-9A-Za-z]|$))")},
      {"bank_card", std::regex(R"((^|[^0-9])[0-9]{16,19}([^0-9]|$))")},
      {"email", std::regex(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})")},
      {"exact_address", std::regex(R"((路|街|巷|弄|道)[[:space:]]*[0-9]+[[:space:]]*号)")}};
  return patterns;
}

}  // namespace

QueryPrivacyResult QueryPrivacyGuard::inspect(const std::string& query) const
{
  QueryPrivacyResult result;
  if (query.empty()) {
    result.safe = false;
    result.risk_types.push_back("empty_query");
    return result;
  }

  for (const auto& [risk_type, pattern] : riskPatterns()) {
    if (std::regex_search(query, pattern)) {
      result.risk_types.push_back(risk_type);
    }
  }
  result.safe = result.risk_types.empty();
  return result;
}

}  // namespace qaiservice::web_search
