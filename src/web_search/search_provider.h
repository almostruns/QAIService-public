#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace qaiservice::web_search {

enum class SearchRecency {
  kAny,
  kWeek,
  kMonth,
  kSemiYear,
  kYear,
};

struct SearchRequest {
  std::string query;
  SearchRecency recency{SearchRecency::kAny};
  std::size_t maximum_results{10};
  std::chrono::milliseconds maximum_duration{0};
};

struct SearchResult {
  std::string title;
  std::string url;
  std::string site;
  std::string snippet;
  std::string published_at;
};

enum class SearchStatus {
  kSuccess,
  kNotConfigured,
  kTimeout,
  kRateLimited,
  kQuotaExhausted,
  kUnauthorized,
  kUpstreamError,
  kInvalidResponse,
};

struct SearchResponse {
  SearchStatus status{SearchStatus::kUpstreamError};
  std::vector<SearchResult> results;
};

class SearchProvider {
public:
  virtual ~SearchProvider() = default;

  [[nodiscard]] virtual SearchResponse search(const SearchRequest& request) = 0;
};

}  // namespace qaiservice::web_search
