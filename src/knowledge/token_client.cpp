#include "knowledge/token_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace qaiservice::knowledge {
namespace {

constexpr std::size_t kMaximumResponseBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumUtf8BoundaryTokenExpansion = 8;

class CurlTokenTransport final : public TokenTransport {
 public:
  [[nodiscard]] TokenHttpResponse perform(const TokenHttpRequest& request) override;
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
  bool exceeded_limit{false};
};

void initializeCurl()
{
  static std::once_flag initialization;
  std::call_once(initialization, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* context)
{
  const std::size_t byte_count = size * count;
  auto* buffer = static_cast<ResponseBuffer*>(context);
  if (buffer->body.size() + byte_count > kMaximumResponseBytes) {
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

TokenStatus responseStatus(const TokenHttpResponse& response)
{
  if (response.status == TokenHttpStatus::kTimeout) {
    return TokenStatus::kTimeout;
  }
  if (response.status == TokenHttpStatus::kUnavailable) {
    return TokenStatus::kUnavailable;
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    return TokenStatus::kUpstreamError;
  }
  return TokenStatus::kSuccess;
}

bool isRequestedPrefix(const std::vector<std::string>& requested, const std::vector<std::string>& returned)
{
  if (returned.size() > requested.size()) {
    return false;
  }
  for (std::size_t index = 0; index < returned.size(); ++index) {
    if (index + 1 < returned.size() && returned[index] != requested[index]) {
      return false;
    }
    if (index + 1 == returned.size() && requested[index].compare(0, returned[index].size(), returned[index]) != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

TokenHttpResponse CurlTokenTransport::perform(const TokenHttpRequest& request)
{
  initializeCurl();
  std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
  if (handle == nullptr) {
    return {TokenHttpStatus::kUnavailable, 0, ""};
  }
  curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
  std::unique_ptr<curl_slist, CurlHeaderDeleter> headers(raw_headers);
  ResponseBuffer response_buffer;
  const long timeout_ms = request.timeout.count();
  const long connect_timeout_ms = std::min(timeout_ms, 1000L);
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request.body.data());
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, appendResponse);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response_buffer);
  const CURLcode result = curl_easy_perform(handle.get());
  if (result == CURLE_OPERATION_TIMEDOUT) {
    return {TokenHttpStatus::kTimeout, 0, ""};
  }
  if (result != CURLE_OK || response_buffer.exceeded_limit) {
    return {TokenHttpStatus::kUnavailable, 0, ""};
  }
  long status_code = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
  return {TokenHttpStatus::kSuccess, status_code, std::move(response_buffer.body)};
}

TokenConfig tokenConfigFromEnvironment()
{
  const char* base_url = std::getenv("QAI_TOKEN_BASE_URL");
  TokenConfig config;
  config.enabled = environmentFlag("QAI_TOKEN_ENABLED");
  config.base_url = base_url == nullptr ? "" : base_url;
  config.timeout = environmentTimeout("QAI_TOKEN_TIMEOUT_MS", std::chrono::milliseconds{5000});
  if (config.base_url.empty()) {
    config.enabled = false;
  }
  return config;
}

TokenClient::TokenClient(TokenConfig config)
    : TokenClient(std::move(config), std::make_shared<CurlTokenTransport>())
{
}

TokenClient::TokenClient(TokenConfig config, std::shared_ptr<TokenTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport))
{
}

TokenSplitResult TokenClient::split(const std::string& text, std::size_t chunk_tokens,
                                    std::size_t overlap_tokens) const
{
  if (!config_.enabled || text.empty()) {
    return {TokenStatus::kDisabled};
  }
  TokenHttpRequest request;
  request.url = config_.base_url + "/chunks";
  request.body = nlohmann::json{{"text", text},
                                {"chunk_tokens", chunk_tokens},
                                {"overlap_tokens", overlap_tokens}}
                     .dump();
  request.timeout = config_.timeout;
  const TokenHttpResponse response = transport_->perform(request);
  const TokenStatus status = responseStatus(response);
  if (status != TokenStatus::kSuccess) {
    return {status};
  }
  try {
    const nlohmann::json body = nlohmann::json::parse(response.body);
    if (!body.contains("encoding") || !body["encoding"].is_string() || !body.contains("chunks") ||
        !body["chunks"].is_array() || !body.contains("token_counts") || !body["token_counts"].is_array() ||
        body["chunks"].empty() || body["chunks"].size() != body["token_counts"].size()) {
      return {TokenStatus::kInvalidResponse};
    }
    std::vector<std::string> chunks;
    std::vector<std::size_t> token_counts;
    for (std::size_t index = 0; index < body["chunks"].size(); ++index) {
      if (!body["chunks"][index].is_string() || body["chunks"][index].get_ref<const std::string&>().empty() ||
          !body["token_counts"][index].is_number_unsigned()) {
        return {TokenStatus::kInvalidResponse};
      }
      const std::size_t token_count = body["token_counts"][index].get<std::size_t>();
      const bool excessive_alignment =
          token_count > chunk_tokens && token_count - chunk_tokens > kMaximumUtf8BoundaryTokenExpansion;
      if (token_count == 0 || excessive_alignment) {
        return {TokenStatus::kInvalidResponse};
      }
      chunks.push_back(body["chunks"][index].get<std::string>());
      token_counts.push_back(token_count);
    }
    return {TokenStatus::kSuccess, body["encoding"].get<std::string>(), std::move(chunks),
            std::move(token_counts)};
  } catch (const nlohmann::json::exception&) {
    return {TokenStatus::kInvalidResponse};
  }
}

TokenFitResult TokenClient::fit(const std::vector<std::string>& texts, std::size_t maximum_tokens) const
{
  if (!config_.enabled || texts.empty()) {
    return {TokenStatus::kDisabled};
  }
  TokenHttpRequest request;
  request.url = config_.base_url + "/fit";
  request.body = nlohmann::json{{"texts", texts}, {"maximum_tokens", maximum_tokens}}.dump();
  request.timeout = config_.timeout;
  const TokenHttpResponse response = transport_->perform(request);
  const TokenStatus status = responseStatus(response);
  if (status != TokenStatus::kSuccess) {
    return {status};
  }
  try {
    const nlohmann::json body = nlohmann::json::parse(response.body);
    if (!body.contains("encoding") || !body["encoding"].is_string() || !body.contains("texts") ||
        !body["texts"].is_array() || !body.contains("token_count") || !body["token_count"].is_number_unsigned()) {
      return {TokenStatus::kInvalidResponse};
    }
    const std::size_t token_count = body["token_count"].get<std::size_t>();
    if (token_count > maximum_tokens) {
      return {TokenStatus::kInvalidResponse};
    }
    std::vector<std::string> fitted = body["texts"].get<std::vector<std::string>>();
    if (!isRequestedPrefix(texts, fitted)) {
      return {TokenStatus::kInvalidResponse};
    }
    return {TokenStatus::kSuccess, body["encoding"].get<std::string>(), std::move(fitted), token_count};
  } catch (const nlohmann::json::exception&) {
    return {TokenStatus::kInvalidResponse};
  }
}

}  // namespace qaiservice::knowledge
