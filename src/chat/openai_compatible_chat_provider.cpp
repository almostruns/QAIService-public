#include "chat/openai_compatible_chat_provider.h"

#include "chat/chat_names.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace qaiservice::chat {
namespace {

constexpr std::size_t kMaximumResponseBytes = 1024 * 1024;

class CurlHttpTransport final : public HttpTransport {
 public:
  HttpResponse perform(const HttpRequest& request) override;
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

HttpRequest makeRequest(const ChatModelConfig& config, const std::vector<ChatMessage>& messages)
{
  nlohmann::json request_messages = nlohmann::json::array();
  for (const ChatMessage& message : messages) {
    request_messages.push_back({{"role", chat::chatRoleName(message.role)}, {"content", message.content}});
  }

  nlohmann::json body{{"model", config.model}, {"messages", std::move(request_messages)}};
  HttpRequest request;
  request.url = config.endpoint;
  request.headers["Authorization"] = "Bearer " + config.api_key;
  request.headers["Content-Type"] = "application/json";
  request.body = body.dump();
  request.timeout = config.timeout;
  return request;
}

}  // namespace

HttpResponse CurlHttpTransport::perform(const HttpRequest& request)
{
  initializeCurl();
  std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
  if (handle == nullptr) {
    return {HttpTransportStatus::kUnavailable, 0, ""};
  }

  curl_slist* header_list = nullptr;
  for (const auto& [name, value] : request.headers) {
    const std::string header = name + ": " + value;
    header_list = curl_slist_append(header_list, header.c_str());
  }
  std::unique_ptr<curl_slist, CurlHeaderDeleter> owned_headers(header_list);

  ResponseBuffer response_buffer;
  const long timeout_ms = request.timeout.count();
  const long connect_timeout_ms = std::min(timeout_ms, 5000L);
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, owned_headers.get());
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
    return {HttpTransportStatus::kTimeout, 0, ""};
  }
  if (result != CURLE_OK || response_buffer.exceeded_limit) {
    return {HttpTransportStatus::kUnavailable, 0, ""};
  }

  long status_code = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
  return {HttpTransportStatus::kSuccess, status_code, std::move(response_buffer.body)};
}

OpenAICompatibleChatProvider::OpenAICompatibleChatProvider(ChatModelConfig config)
    : OpenAICompatibleChatProvider(std::move(config), std::make_shared<CurlHttpTransport>())
{
}

OpenAICompatibleChatProvider::OpenAICompatibleChatProvider(ChatModelConfig config,
                                                           std::shared_ptr<HttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport))
{
}

ChatCompletion OpenAICompatibleChatProvider::complete(const std::vector<ChatMessage>& messages)
{
  if (messages.empty()) {
    return {ChatCompletionStatus::kInvalidRequest};
  }

  const HttpResponse response = transport_->perform(makeRequest(config_, messages));
  if (response.status == HttpTransportStatus::kTimeout) {
    return {ChatCompletionStatus::kTimeout};
  }
  if (response.status == HttpTransportStatus::kUnavailable) {
    return {ChatCompletionStatus::kUnavailable};
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    return {ChatCompletionStatus::kUpstreamError};
  }

  try {
    const nlohmann::json body = nlohmann::json::parse(response.body);
    if (!body.contains("choices") || !body["choices"].is_array() || body["choices"].empty()) {
      return {ChatCompletionStatus::kInvalidResponse};
    }
    const nlohmann::json& message = body["choices"][0]["message"];
    if (!message.contains("content") || !message["content"].is_string()) {
      return {ChatCompletionStatus::kInvalidResponse};
    }
    const std::string content = message["content"].get<std::string>();
    if (content.empty()) {
      return {ChatCompletionStatus::kInvalidResponse};
    }
    return {ChatCompletionStatus::kSuccess, {ChatRole::kAssistant, content}};
  } catch (const nlohmann::json::exception&) {
    return {ChatCompletionStatus::kInvalidResponse};
  }
}

}  // namespace qaiservice::chat
