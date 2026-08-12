#include "web_search/web_cache.h"

#include <utility>

namespace qaiservice::web_search {

SearchResponseCache::SearchResponseCache(Clock clock, std::int64_t lifetime_ms, std::size_t capacity)
    : cache_(std::move(clock), lifetime_ms, capacity)
{
}

std::optional<SearchResponse> SearchResponseCache::get(const std::string& query)
{
  return cache_.get(query);
}

void SearchResponseCache::put(const std::string& query, SearchResponse response, bool sensitive)
{
  if (sensitive || response.status != SearchStatus::kSuccess) {
    return;
  }
  cache_.put(query, std::move(response));
}

PageBodyCache::PageBodyCache(Clock clock, std::int64_t lifetime_ms, std::size_t capacity)
    : cache_(std::move(clock), lifetime_ms, capacity)
{
}

std::optional<std::string> PageBodyCache::get(const std::string& url)
{
  return cache_.get(url);
}

void PageBodyCache::put(const std::string& url, std::string body)
{
  cache_.put(url, std::move(body));
}

}  // namespace qaiservice::web_search
