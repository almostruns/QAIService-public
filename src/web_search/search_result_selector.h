#pragma once

#include "web_search/search_provider.h"

#include <cstddef>
#include <string>
#include <vector>

namespace qaiservice::web_search {

class SearchResultSelector {
public:
  [[nodiscard]] std::vector<SearchResult> select(const std::string& query,
                                                 const std::vector<SearchResult>& candidates,
                                                 std::size_t maximum_results) const;
};

}  // namespace qaiservice::web_search
