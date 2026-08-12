#pragma once

#include "web_search/search_provider.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace qaiservice::web_search {

enum class SearchHttpStatus {
  kSuccess,
  kTimeout,
  kUnavailable,
};

struct SearchHttpRequest {
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds timeout{0};
  std::size_t maximum_response_bytes{0};
};

struct SearchHttpResponse {
  SearchHttpStatus status{SearchHttpStatus::kUnavailable};
  long status_code{0};
  std::string body;
};

class SearchHttpTransport {
public:
  virtual ~SearchHttpTransport() = default;

  [[nodiscard]] virtual SearchHttpResponse perform(const SearchHttpRequest& request) = 0;
};

struct BaiduSearchConfig {
  bool enabled{false};
  std::string api_key;
  std::chrono::milliseconds connect_timeout{1000};
  std::chrono::milliseconds timeout{5000};
  std::size_t top_k{10};
};

[[nodiscard]] BaiduSearchConfig baiduSearchConfigFromEnvironment();

class BaiduSearchProvider final : public SearchProvider {
public:
  explicit BaiduSearchProvider(BaiduSearchConfig config);
  BaiduSearchProvider(BaiduSearchConfig config, std::shared_ptr<SearchHttpTransport> transport);

  [[nodiscard]] SearchResponse search(const SearchRequest& request) override;

private:
  BaiduSearchConfig config_;
  std::shared_ptr<SearchHttpTransport> transport_;
};

}  // namespace qaiservice::web_search
