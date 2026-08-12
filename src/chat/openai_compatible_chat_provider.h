#pragma once

#include "chat/chat_model_config.h"
#include "chat/chat_model_provider.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace qaiservice::chat {

enum class HttpTransportStatus {
  kSuccess,
  kTimeout,
  kUnavailable,
};

struct HttpRequest {
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  std::chrono::milliseconds timeout{0};
};

struct HttpResponse {
  HttpTransportStatus status{HttpTransportStatus::kUnavailable};
  long status_code{0};
  std::string body;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;

  [[nodiscard]] virtual HttpResponse perform(const HttpRequest& request) = 0;
};

class OpenAICompatibleChatProvider final : public ChatModelProvider {
 public:
  explicit OpenAICompatibleChatProvider(ChatModelConfig config);
  OpenAICompatibleChatProvider(ChatModelConfig config, std::shared_ptr<HttpTransport> transport);

  [[nodiscard]] ChatCompletion complete(const std::vector<ChatMessage>& messages) override;

 private:
  ChatModelConfig config_;
  std::shared_ptr<HttpTransport> transport_;
};

}  // namespace qaiservice::chat
