#pragma once

#include "web_search/search_provider.h"

#include <functional>

namespace qaiservice::web_search {

class MockSearchProvider final : public SearchProvider {
public:
  using Handler = std::function<SearchResponse(const SearchRequest&)>;

  explicit MockSearchProvider(Handler handler);
  [[nodiscard]] SearchResponse search(const SearchRequest& request) override;

private:
  Handler handler_;
};

}  // namespace qaiservice::web_search
