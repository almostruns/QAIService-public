#pragma once

#include <string>
#include <vector>

namespace qaiservice::chat {

enum class ConversationMode {
  kGeneral,
  kPrivate,
};

enum class ChatRole {
  kSystem,
  kUser,
  kAssistant,
};

struct ChatMessage {
  ChatRole role;
  std::string content;
};

enum class ChatCompletionStatus {
  kSuccess,
  kInvalidRequest,
  kTimeout,
  kUnavailable,
  kUpstreamError,
  kInvalidResponse,
};

enum class ChatPersistenceStatus {
  kNotConfigured,
  kQueued,
  kBusy,
  kUnavailable,
};

struct ChatCompletion {
  ChatCompletionStatus status;
  ChatMessage message{ChatRole::kAssistant, ""};
  ChatPersistenceStatus persistence{ChatPersistenceStatus::kNotConfigured};
};

class ChatModelProvider {
 public:
  virtual ~ChatModelProvider() = default;

  [[nodiscard]] virtual ChatCompletion complete(const std::vector<ChatMessage>& messages) = 0;
};

}  // namespace qaiservice::chat
