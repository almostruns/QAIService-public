#pragma once

#include "web_search/url_safety.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qaiservice::web_search {

enum class PageHttpStatus {
  kSuccess,
  kTimeout,
  kUnavailable,
  kTooLarge,
};

struct PageHttpRequest {
  std::string url;
  std::string host;
  std::uint16_t port{0};
  std::vector<std::string> pinned_ips;
  std::map<std::string, std::string> headers;
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds timeout{0};
  std::size_t maximum_response_bytes{0};
};

struct PageHttpResponse {
  PageHttpStatus status{PageHttpStatus::kUnavailable};
  long status_code{0};
  std::string content_type;
  std::string body;
  std::string location;
};

class PageTransport {
public:
  virtual ~PageTransport() = default;

  [[nodiscard]] virtual PageHttpResponse perform(const PageHttpRequest& request) = 0;
};

enum class PageFetchStatus {
  kSuccess,
  kUnsafeUrl,
  kTimeout,
  kTooLarge,
  kUnsupportedContent,
  kRedirectLimit,
  kUpstreamError,
};

struct PageFetchResult {
  PageFetchStatus status{PageFetchStatus::kUpstreamError};
  std::string final_url;
  std::string content_type;
  std::string body;
};

struct SafePageFetcherConfig {
  std::chrono::milliseconds connect_timeout{1000};
  std::chrono::milliseconds timeout{5000};
  std::size_t maximum_response_bytes{2 * 1024 * 1024};
  std::size_t maximum_redirects{3};
};

class SafePageFetcher {
public:
  SafePageFetcher(UrlSafety safety, SafePageFetcherConfig config);
  SafePageFetcher(UrlSafety safety, std::shared_ptr<PageTransport> transport, SafePageFetcherConfig config);

  [[nodiscard]] PageFetchResult fetch(const std::string& url,
                                      std::chrono::milliseconds maximum_duration = std::chrono::milliseconds{0}) const;

private:
  UrlSafety safety_;
  std::shared_ptr<PageTransport> transport_;
  SafePageFetcherConfig config_;
};

}  // namespace qaiservice::web_search
