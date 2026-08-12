#pragma once

#include "web_search/web_search_types.h"

#include <string>

namespace qaiservice::web_search {

class QueryPrivacyGuard {
public:
  QueryPrivacyResult inspect(const std::string& query) const;
};

}  // namespace qaiservice::web_search
