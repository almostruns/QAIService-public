#include "web_search/safe_page_fetcher.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace qaiservice::web_search {
namespace {

class CurlPageTransport final : public PageTransport {
public:
  [[nodiscard]] PageHttpResponse perform(const PageHttpRequest& request) override;
};

struct CurlHandleDeleter {
  void operator()(CURL* handle) const
  {
    curl_easy_cleanup(handle);
  }
};

struct CurlListDeleter {
  void operator()(curl_slist* list) const
  {
    curl_slist_free_all(list);
  }
};

struct ResponseBuffer {
  std::string body;
  std::string location;
  std::size_t maximum_bytes{0};
  bool exceeded_limit{false};
};

void initializeCurl()
{
  static std::once_flag initialization;
  std::call_once(initialization, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("failed to initialize curl");
    }
  });
}

std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* context)
{
  const std::size_t byte_count = size * count;
  auto* buffer = static_cast<ResponseBuffer*>(context);
  if (buffer->body.size() + byte_count > buffer->maximum_bytes) {
    buffer->exceeded_limit = true;
    return 0;
  }
  buffer->body.append(data, byte_count);
  return byte_count;
}

std::size_t inspectHeader(char* data, std::size_t size, std::size_t count, void* context)
{
  const std::size_t byte_count = size * count;
  const std::string line(data, byte_count);
  auto* buffer = static_cast<ResponseBuffer*>(context);
  if (line.size() >= 9) {
    std::string name = line.substr(0, 9);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (name == "location:") {
      std::size_t start = 9;
      while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])) != 0) {
        ++start;
      }
      std::size_t end = line.find_last_not_of("\r\n ");
      buffer->location = end < start ? "" : line.substr(start, end - start + 1);
    }
  }
  return byte_count;
}

bool redirectStatus(long status_code)
{
  return status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308;
}

bool supportedContentType(const std::string& content_type)
{
  std::string normalized = content_type;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return normalized.rfind("text/html", 0) == 0 || normalized.rfind("text/plain", 0) == 0;
}

bool retryable(const PageHttpResponse& response)
{
  if (response.status == PageHttpStatus::kTimeout || response.status == PageHttpStatus::kUnavailable) {
    return true;
  }
  return response.status == PageHttpStatus::kSuccess &&
         (response.status_code == 500 || response.status_code == 502 || response.status_code == 503 ||
          response.status_code == 504);
}

std::string canonicalUrl(const SafeUrlTarget& target)
{
  std::string result = target.scheme + "://" + target.host;
  if ((target.scheme == "http" && target.port != 80) || (target.scheme == "https" && target.port != 443)) {
    result += ":" + std::to_string(target.port);
  }
  return result + target.path;
}

std::string redirectUrl(const SafeUrlTarget& current, const std::string& location)
{
  if (location.find("://") != std::string::npos) {
    return location;
  }
  const std::string origin = current.scheme + "://" + current.host;
  if (!location.empty() && location.front() == '/') {
    return origin + location;
  }
  const std::size_t slash = current.path.rfind('/');
  const std::string directory = slash == std::string::npos ? "/" : current.path.substr(0, slash + 1);
  return origin + directory + location;
}

}  // namespace

PageHttpResponse CurlPageTransport::perform(const PageHttpRequest& request)
{
  initializeCurl();
  std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
  if (handle == nullptr) {
    return {};
  }

  curl_slist* raw_headers = nullptr;
  for (const auto& [name, value] : request.headers) {
    const std::string header = name + ": " + value;
    raw_headers = curl_slist_append(raw_headers, header.c_str());
  }
  std::unique_ptr<curl_slist, CurlListDeleter> headers(raw_headers);
  curl_slist* raw_resolve = nullptr;
  for (const std::string& ip : request.pinned_ips) {
    const bool ipv6 = ip.find(':') != std::string::npos;
    const std::string address = ipv6 ? "[" + ip + "]" : ip;
    const std::string entry = request.host + ":" + std::to_string(request.port) + ":" + address;
    raw_resolve = curl_slist_append(raw_resolve, entry.c_str());
  }
  std::unique_ptr<curl_slist, CurlListDeleter> resolve(raw_resolve);

  ResponseBuffer buffer;
  buffer.maximum_bytes = request.maximum_response_bytes;
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_RESOLVE, resolve.get());
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, inspectHeader);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &buffer);

  const CURLcode result = curl_easy_perform(handle.get());
  if (result == CURLE_OPERATION_TIMEDOUT) {
    return {PageHttpStatus::kTimeout};
  }
  if (buffer.exceeded_limit) {
    return {PageHttpStatus::kTooLarge};
  }
  if (result != CURLE_OK) {
    return {PageHttpStatus::kUnavailable};
  }
  long status_code = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
  char* raw_content_type = nullptr;
  curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &raw_content_type);
  const std::string content_type = raw_content_type == nullptr ? "" : raw_content_type;
  return {PageHttpStatus::kSuccess, status_code, content_type, std::move(buffer.body), std::move(buffer.location)};
}

SafePageFetcher::SafePageFetcher(UrlSafety safety, SafePageFetcherConfig config)
    : SafePageFetcher(std::move(safety), std::make_shared<CurlPageTransport>(), config)
{
}

SafePageFetcher::SafePageFetcher(UrlSafety safety, std::shared_ptr<PageTransport> transport,
                                 SafePageFetcherConfig config)
    : safety_(std::move(safety)), transport_(std::move(transport)), config_(config)
{
}

PageFetchResult SafePageFetcher::fetch(const std::string& url, std::chrono::milliseconds maximum_duration) const
{
  const bool bounded = maximum_duration.count() > 0;
  const auto deadline = std::chrono::steady_clock::now() + maximum_duration;
  std::string current_url = url;
  std::set<std::string> visited;
  for (std::size_t redirect_count = 0; redirect_count <= config_.maximum_redirects; ++redirect_count) {
    const auto target = safety_.inspect(current_url);
    if (!target.has_value()) {
      return {PageFetchStatus::kUnsafeUrl};
    }
    current_url = canonicalUrl(target.value());
    if (!visited.insert(current_url).second) {
      return {PageFetchStatus::kRedirectLimit};
    }

    PageHttpRequest request;
    request.url = current_url;
    request.host = target->host;
    request.port = target->port;
    request.pinned_ips = target->validated_ips;
    request.headers["Accept"] = "text/html,text/plain;q=0.9";
    request.headers["User-Agent"] = "QAIService-WebEvidence/1.0";
    if (bounded) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return {PageFetchStatus::kTimeout};
      }
      request.timeout = std::min(config_.timeout, remaining);
    } else {
      request.timeout = config_.timeout;
    }
    request.connect_timeout = std::min(config_.connect_timeout, request.timeout);
    request.maximum_response_bytes = config_.maximum_response_bytes;
    PageHttpResponse response = transport_->perform(request);
    if (retryable(response)) {
      if (bounded && std::chrono::steady_clock::now() >= deadline) {
        return {PageFetchStatus::kTimeout};
      }
      const auto backoff = bounded ? std::min(std::chrono::milliseconds{50},
                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  deadline - std::chrono::steady_clock::now()))
                                   : std::chrono::milliseconds{50};
      std::this_thread::sleep_for(std::max(backoff, std::chrono::milliseconds{0}));
      if (bounded) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          return {PageFetchStatus::kTimeout};
        }
        request.timeout = std::min(config_.timeout, remaining);
        request.connect_timeout = std::min(config_.connect_timeout, request.timeout);
      }
      response = transport_->perform(request);
    }

    if (response.status == PageHttpStatus::kTimeout) {
      return {PageFetchStatus::kTimeout};
    }
    if (response.status == PageHttpStatus::kTooLarge) {
      return {PageFetchStatus::kTooLarge};
    }
    if (response.status != PageHttpStatus::kSuccess) {
      return {PageFetchStatus::kUpstreamError};
    }
    if (redirectStatus(response.status_code)) {
      if (response.location.empty() || redirect_count == config_.maximum_redirects) {
        return {PageFetchStatus::kRedirectLimit};
      }
      current_url = redirectUrl(target.value(), response.location);
      continue;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
      return {PageFetchStatus::kUpstreamError};
    }
    if (!supportedContentType(response.content_type)) {
      return {PageFetchStatus::kUnsupportedContent};
    }
    return {PageFetchStatus::kSuccess, current_url, response.content_type, response.body};
  }
  return {PageFetchStatus::kRedirectLimit};
}

}  // namespace qaiservice::web_search
