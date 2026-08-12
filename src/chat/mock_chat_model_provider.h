#pragma once

#include "chat/chat_model_provider.h"

namespace qaiservice::chat {

class MockChatModelProvider final : public ChatModelProvider {
 public:
  [[nodiscard]] ChatCompletion complete(const std::vector<ChatMessage>& messages) override;
};

}  // namespace qaiservice::chat
