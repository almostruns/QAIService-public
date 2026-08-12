#pragma once

#include "chat/chat_model_provider.h"

#include <cstdint>
#include <optional>
#include <string>

namespace qaiservice::persistence {

struct ChatMessageCreated {
  std::string event_id;
  std::uint64_t user_id;
  std::uint64_t sequence;
  chat::ChatRole role;
  std::string content;
  std::int64_t occurred_at_ms;
  chat::ConversationMode mode{chat::ConversationMode::kGeneral};
  std::uint64_t conversation_id{0};
};

[[nodiscard]] std::string makeEventId();
[[nodiscard]] std::string chatMessageEventToJson(const ChatMessageCreated& event);
[[nodiscard]] std::optional<ChatMessageCreated> chatMessageEventFromJson(const std::string& json);

}  // namespace qaiservice::persistence
