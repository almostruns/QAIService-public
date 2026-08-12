#include "web_search/mock_search_provider.h"

#include <utility>

namespace qaiservice::web_search {

MockSearchProvider::MockSearchProvider(Handler handler) : handler_(std::move(handler))
{
}

SearchResponse MockSearchProvider::search(const SearchRequest& request)
{
  if (!handler_) {
    return {SearchStatus::kNotConfigured};
  }
  return handler_(request);
}

}  // namespace qaiservice::web_search
