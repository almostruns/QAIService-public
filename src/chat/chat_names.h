#pragma once

#include "chat/chat_model_provider.h"

#include <optional>
#include <string_view>

namespace qaiservice::chat {

inline const char* chatRoleName(ChatRole role)
{
  switch (role) {
    case ChatRole::kSystem:
      return "system";
    case ChatRole::kUser:
      return "user";
    case ChatRole::kAssistant:
      return "assistant";
  }
  return "unknown";
}

inline std::optional<ChatRole> parseChatRole(std::string_view role)
{
  if (role == "user") {
    return ChatRole::kUser;
  }
  if (role == "assistant") {
    return ChatRole::kAssistant;
  }
  if (role == "system") {
    return ChatRole::kSystem;
  }
  return std::nullopt;
}

inline const char* conversationModeName(ConversationMode mode)
{
  return mode == ConversationMode::kPrivate ? "private" : "general";
}

inline std::optional<ConversationMode> parseConversationMode(std::string_view mode)
{
  if (mode == "general") {
    return ConversationMode::kGeneral;
  }
  if (mode == "private") {
    return ConversationMode::kPrivate;
  }
  return std::nullopt;
}

}  // namespace qaiservice::chat
