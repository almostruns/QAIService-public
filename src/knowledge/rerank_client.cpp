#include "knowledge/rerank_client.h"

#include "util/utf8.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace qaiservice::knowledge {
namespace {

constexpr std::size_t kMaximumCandidates = 64;
constexpr std::size_t kMaximumCandidateBytes = 2000;
constexpr std::size_t kMaximumResponseBytes = 1024 * 1024;

class CurlRerankTransport final : public RerankTransport {
 public:
  [[nodiscard]] RerankHttpResponse perform(const RerankHttpRequest& request) override;
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

std::string candidateId(const SearchCandidate& candidate)
{
  return std::to_string(candidate.document_id) + ":" + std::to_string(candidate.chunk_index);
}

std::string candidateText(const SearchCandidate& candidate)
{
  std::string text = "文件：" + candidate.filename;
  if (candidate.page_number.has_value()) {
    text += "\n页码：" + std::to_string(candidate.page_number.value());
  }
  text += "\n内容：\n" + candidate.content;
  if (text.size() > kMaximumCandidateBytes) {
    text.resize(util::utf8Boundary(text, kMaximumCandidateBytes));
  }
  return text;
}

std::optional<std::pair<std::uint64_t, std::uint32_t>> parseCandidateId(const std::string& id)
{
  const std::size_t separator = id.find(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == id.size()) {
    return std::nullopt;
  }
  std::uint64_t document_id = 0;
  std::uint32_t chunk_index = 0;
  const auto document = std::from_chars(id.data(), id.data() + separator, document_id);
  const auto chunk = std::from_chars(id.data() + separator + 1, id.data() + id.size(), chunk_index);
  if (document.ec != std::errc{} || document.ptr != id.data() + separator || chunk.ec != std::errc{} ||
      chunk.ptr != id.data() + id.size()) {
    return std::nullopt;
  }
  return std::make_pair(document_id, chunk_index);
}

RerankHttpRequest makeRequest(const RerankConfig& config, const std::string& query,
                              const std::vector<SearchCandidate>& candidates)
{
  nlohmann::json request_candidates = nlohmann::json::array();
  const std::size_t candidate_count = std::min(kMaximumCandidates, candidates.size());
  for (std::size_t index = 0; index < candidate_count; ++index) {
    request_candidates.push_back({{"id", candidateId(candidates[index])}, {"text", candidateText(candidates[index])}});
  }
  RerankHttpRequest request;
  request.url = config.endpoint;
  request.body = nlohmann::json{{"query", query}, {"candidates", std::move(request_candidates)}}.dump();
  request.timeout = config.timeout;
  return request;
}

}  // namespace

RerankHttpResponse CurlRerankTransport::perform(const RerankHttpRequest& request)
{
  initializeCurl();
  std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
  if (handle == nullptr) {
    return {RerankHttpStatus::kUnavailable, 0, ""};
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
    return {RerankHttpStatus::kTimeout, 0, ""};
  }
  if (result != CURLE_OK || response_buffer.exceeded_limit) {
    return {RerankHttpStatus::kUnavailable, 0, ""};
  }
  long status_code = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
  return {RerankHttpStatus::kSuccess, status_code, std::move(response_buffer.body)};
}

RerankConfig rerankConfigFromEnvironment()
{
  const char* endpoint = std::getenv("QAI_RERANK_URL");
  RerankConfig config;
  config.enabled = environmentFlag("QAI_RERANK_ENABLED");
  config.endpoint = endpoint == nullptr ? "" : endpoint;
  config.timeout = environmentTimeout("QAI_RERANK_TIMEOUT_MS", std::chrono::milliseconds{3000});
  if (config.endpoint.empty()) {
    config.enabled = false;
  }
  return config;
}

RerankClient::RerankClient(RerankConfig config)
    : RerankClient(std::move(config), std::make_shared<CurlRerankTransport>())
{
}

RerankClient::RerankClient(RerankConfig config, std::shared_ptr<RerankTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport))
{
}

RerankResult RerankClient::rerank(const std::string& query, const std::vector<SearchCandidate>& candidates) const
{
  if (!config_.enabled || query.empty() || candidates.empty()) {
    return {RerankStatus::kDisabled};
  }
  const RerankHttpResponse response = transport_->perform(makeRequest(config_, query, candidates));
  if (response.status == RerankHttpStatus::kTimeout) {
    return {RerankStatus::kTimeout};
  }
  if (response.status == RerankHttpStatus::kUnavailable) {
    return {RerankStatus::kUnavailable};
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    return {RerankStatus::kUpstreamError};
  }
  try {
    const nlohmann::json body = nlohmann::json::parse(response.body);
    if (!body.contains("model") || !body["model"].is_string() || !body.contains("scores") ||
        !body["scores"].is_array()) {
      return {RerankStatus::kInvalidResponse};
    }
    const std::size_t expected_count = std::min(kMaximumCandidates, candidates.size());
    if (body["scores"].size() != expected_count) {
      return {RerankStatus::kInvalidResponse};
    }
    std::unordered_set<std::string> expected_ids;
    for (std::size_t index = 0; index < expected_count; ++index) {
      expected_ids.insert(candidateId(candidates[index]));
    }
    std::unordered_set<std::string> returned_ids;
    std::vector<RerankScore> scores;
    scores.reserve(expected_count);
    for (const nlohmann::json& item : body["scores"]) {
      if (!item.contains("id") || !item["id"].is_string() || !item.contains("score") || !item["score"].is_number()) {
        return {RerankStatus::kInvalidResponse};
      }
      const double score = item["score"].get<double>();
      if (score < 0.0 || score > 1.0) {
        return {RerankStatus::kInvalidResponse};
      }
      const std::string id = item["id"].get<std::string>();
      const auto parsed_id = parseCandidateId(id);
      if (!parsed_id.has_value() || expected_ids.find(id) == expected_ids.end() || !returned_ids.insert(id).second) {
        return {RerankStatus::kInvalidResponse};
      }
      scores.push_back({parsed_id->first, parsed_id->second, score});
    }
    return {RerankStatus::kSuccess, body["model"].get<std::string>(), std::move(scores)};
  } catch (const nlohmann::json::exception&) {
    return {RerankStatus::kInvalidResponse};
  }
}

}  // namespace qaiservice::knowledge
