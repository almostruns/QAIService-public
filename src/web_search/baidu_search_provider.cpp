#include "web_search/baidu_search_provider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace qaiservice::web_search {
namespace {

constexpr char kBaiduSearchEndpoint[] = "https://qianfan.baidubce.com/v2/ai_search/web_search";
constexpr std::size_t kMaximumResponseBytes = 1024 * 1024;
constexpr std::size_t kMaximumBaiduResults = 50;

class CurlSearchHttpTransport final : public SearchHttpTransport {
public:
  [[nodiscard]] SearchHttpResponse perform(const SearchHttpRequest& request) override;
};

struct CurlHandleDeleter {
  void operator()(CURL* handle) const
  {
    curl_easy_cleanup(handle);
  }
};

struct CurlHeaderDeleter {
  void operator()(curl_slist* headers) const
  {
    curl_slist_free_all(headers);
  }
};

struct ResponseBuffer {
  std::string body;
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

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* context)
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

bool environmentFlag(const char* name)
{
  const char* value = std::getenv(name);
  return value != nullptr && (std::string(value) == "true" || std::string(value) == "1");
}

std::chrono::milliseconds environmentTimeout(const char* name, std::chrono::milliseconds fallback)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  long milliseconds = 0;
  const std::string text(value);
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || milliseconds <= 0) {
    return fallback;
  }
  return std::chrono::milliseconds{milliseconds};
}

std::size_t environmentSize(const char* name, std::size_t fallback, std::size_t maximum)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  std::size_t parsed_value = 0;
  const std::string text(value);
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsed_value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || parsed_value == 0) {
    return fallback;
  }
  return std::min(parsed_value, maximum);
}

std::optional<std::string> recencyName(SearchRecency recency)
{
  switch (recency) {
    case SearchRecency::kAny:
      return std::nullopt;
    case SearchRecency::kWeek:
      return "week";
    case SearchRecency::kMonth:
      return "month";
    case SearchRecency::kSemiYear:
      return "semiyear";
    case SearchRecency::kYear:
      return "year";
  }
  return std::nullopt;
}

SearchHttpRequest makeRequest(const BaiduSearchConfig& config, const SearchRequest& search)
{
  const std::size_t configured_top_k = std::min(config.top_k, kMaximumBaiduResults);
  const std::size_t requested_top_k = std::max<std::size_t>(search.maximum_results, 1);
  const std::size_t top_k = std::min(configured_top_k, requested_top_k);
  nlohmann::json body{{"messages", {{{"role", "user"}, {"content", search.query}}}},
                      {"search_source", "baidu_search_v2"},
                      {"resource_type_filter", {{{"type", "web"}, {"top_k", top_k}}}},
                      {"safe_search", true}};
  const auto recency = recencyName(search.recency);
  if (recency.has_value()) {
    body["search_recency_filter"] = recency.value();
    body["sort"] = {{"priority", "auto"}};
  }

  SearchHttpRequest request;
  request.url = kBaiduSearchEndpoint;
  request.headers["Authorization"] = "Bearer " + config.api_key;
  request.headers["Content-Type"] = "application/json";
  request.body = body.dump();
  request.timeout = search.maximum_duration.count() > 0 ? std::min(config.timeout, search.maximum_duration)
                                                        : config.timeout;
  request.connect_timeout = std::min(config.connect_timeout, request.timeout);
  request.maximum_response_bytes = kMaximumResponseBytes;
  return request;
}

bool retryable(const SearchHttpResponse& response)
{
  if (response.status == SearchHttpStatus::kTimeout || response.status == SearchHttpStatus::kUnavailable) {
    return true;
  }
  return response.status == SearchHttpStatus::kSuccess &&
         (response.status_code == 500 || response.status_code == 502 || response.status_code == 503 ||
          response.status_code == 504);
}

std::string lowercase(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool quotaError(const std::string& response_body)
{
  const auto body = nlohmann::json::parse(response_body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    return false;
  }
  std::string description;
  if (body.contains("code") && body["code"].is_string()) {
    description += body["code"].get<std::string>();
  }
  if (body.contains("message") && body["message"].is_string()) {
    description += " " + body["message"].get<std::string>();
  }
  const std::string normalized = lowercase(description);
  return normalized.find("quota") != std::string::npos || normalized.find("resource_exhausted") != std::string::npos ||
         description.find("额度") != std::string::npos;
}

SearchResponse parseResponse(const SearchHttpResponse& response)
{
  if (response.status == SearchHttpStatus::kTimeout) {
    return {SearchStatus::kTimeout};
  }
  if (response.status == SearchHttpStatus::kUnavailable) {
    return {SearchStatus::kUpstreamError};
  }
  if (response.status_code == 401 || response.status_code == 403) {
    return {SearchStatus::kUnauthorized};
  }
  if (response.status_code == 429) {
    return {SearchStatus::kRateLimited};
  }
  if (quotaError(response.body)) {
    return {SearchStatus::kQuotaExhausted};
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    return {SearchStatus::kUpstreamError};
  }

  const auto body = nlohmann::json::parse(response.body, nullptr, false);
  if (body.is_discarded() || !body.is_object() || !body.contains("references") || !body["references"].is_array()) {
    return {SearchStatus::kInvalidResponse};
  }

  std::vector<SearchResult> results;
  for (const auto& reference : body["references"]) {
    if (!reference.is_object() || !reference.contains("title") || !reference["title"].is_string() ||
        !reference.contains("url") || !reference["url"].is_string()) {
      continue;
    }
    SearchResult result;
    result.title = reference["title"].get<std::string>();
    result.url = reference["url"].get<std::string>();
    if (reference.contains("website") && reference["website"].is_string()) {
      result.site = reference["website"].get<std::string>();
    }
    if (reference.contains("content") && reference["content"].is_string()) {
      result.snippet = reference["content"].get<std::string>();
    }
    if (reference.contains("date") && reference["date"].is_string()) {
      result.published_at = reference["date"].get<std::string>();
    }
    results.push_back(std::move(result));
  }
  if (!body["references"].empty() && results.empty()) {
    return {SearchStatus::kInvalidResponse};
  }
  return {SearchStatus::kSuccess, std::move(results)};
}

}  // namespace

SearchHttpResponse CurlSearchHttpTransport::perform(const SearchHttpRequest& request)
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
  std::unique_ptr<curl_slist, CurlHeaderDeleter> headers(raw_headers);
  ResponseBuffer buffer;
  buffer.maximum_bytes = request.maximum_response_bytes;
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request.body.data());
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, appendResponse);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer);

  const CURLcode result = curl_easy_perform(handle.get());
  if (result == CURLE_OPERATION_TIMEDOUT) {
    return {SearchHttpStatus::kTimeout, 0, ""};
  }
  if (result != CURLE_OK || buffer.exceeded_limit) {
    return {SearchHttpStatus::kUnavailable, 0, ""};
  }
  long status_code = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
  return {SearchHttpStatus::kSuccess, status_code, std::move(buffer.body)};
}

BaiduSearchConfig baiduSearchConfigFromEnvironment()
{
  const char* provider = std::getenv("QAI_WEB_SEARCH_PROVIDER");
  const char* api_key = std::getenv("QAI_BAIDU_SEARCH_API_KEY");
  BaiduSearchConfig config;
  const bool baidu_selected = provider == nullptr || std::string(provider) == "baidu";
  config.enabled = environmentFlag("QAI_WEB_SEARCH_ENABLED") && baidu_selected;
  config.api_key = api_key == nullptr ? "" : api_key;
  config.connect_timeout = environmentTimeout("QAI_WEB_SEARCH_CONNECT_TIMEOUT_MS", std::chrono::milliseconds{1000});
  config.timeout = environmentTimeout("QAI_WEB_SEARCH_TIMEOUT_MS", std::chrono::milliseconds{5000});
  config.top_k = environmentSize("QAI_WEB_SEARCH_TOP_K", 10, kMaximumBaiduResults);
  return config;
}

BaiduSearchProvider::BaiduSearchProvider(BaiduSearchConfig config)
    : BaiduSearchProvider(std::move(config), std::make_shared<CurlSearchHttpTransport>())
{
}

BaiduSearchProvider::BaiduSearchProvider(BaiduSearchConfig config, std::shared_ptr<SearchHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport))
{
}

SearchResponse BaiduSearchProvider::search(const SearchRequest& request)
{
  if (!config_.enabled || config_.api_key.empty() || request.query.empty() || transport_ == nullptr) {
    return {SearchStatus::kNotConfigured};
  }

  SearchHttpRequest http_request = makeRequest(config_, request);
  const bool bounded = request.maximum_duration.count() > 0;
  const auto deadline = std::chrono::steady_clock::now() + request.maximum_duration;
  SearchHttpResponse response = transport_->perform(http_request);
  if (retryable(response)) {
    if (bounded && std::chrono::steady_clock::now() >= deadline) {
      return {SearchStatus::kTimeout};
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
        return {SearchStatus::kTimeout};
      }
      http_request.timeout = std::min(config_.timeout, remaining);
      http_request.connect_timeout = std::min(config_.connect_timeout, http_request.timeout);
    }
    response = transport_->perform(http_request);
  }
  return parseResponse(response);
}

}  // namespace qaiservice::web_search
