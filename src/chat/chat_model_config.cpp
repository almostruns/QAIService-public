#include "chat/chat_model_config.h"

#include "chat/mock_chat_model_provider.h"
#include "chat/openai_compatible_chat_provider.h"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace qaiservice::chat {
namespace {

constexpr long kDefaultTimeoutMilliseconds = 30000;
constexpr long kMinimumTimeoutMilliseconds = 100;
constexpr long kMaximumTimeoutMilliseconds = 120000;

std::string readOrEmpty(const EnvironmentReader& reader, const std::string& name)
{
  const std::optional<std::string> value = reader(name);
  return value.value_or("");
}

long parseTimeout(const std::string& value)
{
  if (value.empty()) {
    return kDefaultTimeoutMilliseconds;
  }

  long timeout = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("QAI_MODEL_TIMEOUT_MS must be an integer");
  }
  if (timeout < kMinimumTimeoutMilliseconds || timeout > kMaximumTimeoutMilliseconds) {
    throw std::invalid_argument("QAI_MODEL_TIMEOUT_MS is outside the supported range");
  }
  return timeout;
}

}  // namespace

ChatModelConfig loadChatModelConfig(const EnvironmentReader& read_environment)
{
  const std::string provider = readOrEmpty(read_environment, "QAI_CHAT_PROVIDER");
  const long timeout = parseTimeout(readOrEmpty(read_environment, "QAI_MODEL_TIMEOUT_MS"));

  if (provider.empty() || provider == "mock") {
    return {ChatProviderKind::kMock, "", "", "", std::chrono::milliseconds{timeout}};
  }
  if (provider != "openai-compatible") {
    throw std::invalid_argument("QAI_CHAT_PROVIDER must be mock or openai-compatible");
  }

  ChatModelConfig config{ChatProviderKind::kOpenAICompatible,
                         readOrEmpty(read_environment, "QAI_MODEL_BASE_URL"),
                         readOrEmpty(read_environment, "QAI_MODEL_NAME"),
                         readOrEmpty(read_environment, "QAI_MODEL_API_KEY"),
                         std::chrono::milliseconds{timeout}};
  if (config.endpoint.empty() || config.model.empty() || config.api_key.empty()) {
    throw std::invalid_argument("real model configuration requires URL, model, and API key");
  }
  return config;
}

ChatModelConfig chatModelConfigFromEnvironment()
{
  EnvironmentReader reader = [](const std::string& name) -> std::optional<std::string> {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
  return loadChatModelConfig(reader);
}

std::unique_ptr<ChatModelProvider> makeChatModelProvider(const ChatModelConfig& config)
{
  if (config.provider_kind == ChatProviderKind::kMock) {
    return std::make_unique<MockChatModelProvider>();
  }
  return std::make_unique<OpenAICompatibleChatProvider>(config);
}

}  // namespace qaiservice::chat
