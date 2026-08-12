#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace qaiservice::chat {

class ChatModelProvider;

enum class ChatProviderKind {
  kMock,
  kOpenAICompatible,
};

struct ChatModelConfig {
  ChatProviderKind provider_kind;
  std::string endpoint;
  std::string model;
  std::string api_key;
  std::chrono::milliseconds timeout;
};

using EnvironmentReader = std::function<std::optional<std::string>(const std::string&)>;

[[nodiscard]] ChatModelConfig loadChatModelConfig(const EnvironmentReader& read_environment);
[[nodiscard]] ChatModelConfig chatModelConfigFromEnvironment();
[[nodiscard]] std::unique_ptr<ChatModelProvider> makeChatModelProvider(const ChatModelConfig& config);

}  // namespace qaiservice::chat
