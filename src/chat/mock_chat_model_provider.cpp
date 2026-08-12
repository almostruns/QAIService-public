#include "chat/mock_chat_model_provider.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace qaiservice::chat {

ChatCompletion MockChatModelProvider::complete(const std::vector<ChatMessage>& messages)
{
  const ChatMessage* last_user_message = nullptr;
  std::size_t user_message_count = 0;
  bool private_assistant = false;

  for (const ChatMessage& message : messages) {
    if (message.role == ChatRole::kSystem && message.content.find("PRIVATE_ASSISTANT_JSON") != std::string::npos) {
      private_assistant = true;
    }
    if (message.role == ChatRole::kUser) {
      last_user_message = &message;
      ++user_message_count;
    }
  }

  if (last_user_message == nullptr || last_user_message->content.empty()) {
    return {ChatCompletionStatus::kInvalidRequest};
  }

  if (private_assistant) {
    nlohmann::json result;
    if (last_user_message->content.find("创建") != std::string::npos &&
        last_user_message->content.find("英语") != std::string::npos) {
      result = {{"type", "proposal"},
                {"message", "我准备创建一个绿色的英语项目，请确认后写入。"},
                {"tool", "create_project"},
                {"arguments", {{"name", "英语"}, {"project_type", "focus"},
                               {"color", "#2F6F55"}, {"note", ""}}}};
    } else if (last_user_message->content.find("支出") != std::string::npos &&
               last_user_message->content.find("餐饮") != std::string::npos) {
      result = {{"type", "proposal"},
                {"message", "我准备记录一笔 25.50 元的餐饮支出，请确认后写入。"},
                {"tool", "record_transaction"},
                {"arguments", {{"direction", "expense"},
                               {"amount_minor", 2550},
                               {"currency", "CNY"},
                               {"category", "餐饮"}}}};
    } else if (last_user_message->content.find("体重") != std::string::npos) {
      result = {{"type", "proposal"},
                {"message", "我准备记录体重 71.0 公斤，请确认后写入。"},
                {"tool", "record_weight"},
                {"arguments", {{"grams", 71000}}}};
    } else {
      result = {{"type", "answer"},
                {"message", "Mock 私人管家已收到：" + last_user_message->content}};
    }
    return {ChatCompletionStatus::kSuccess, {ChatRole::kAssistant, result.dump()}};
  }

  std::string answer = "Mock response (turn ";
  answer += std::to_string(user_message_count);
  answer += "): ";
  answer += last_user_message->content;
  return {ChatCompletionStatus::kSuccess, {ChatRole::kAssistant, std::move(answer)}};
}

}  // namespace qaiservice::chat
