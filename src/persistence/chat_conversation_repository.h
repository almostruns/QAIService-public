#pragma once

#include "chat/chat_model_provider.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::persistence {

struct ChatConversation {
  std::uint64_t id;
  std::uint64_t user_id;
  chat::ConversationMode mode;
  std::string title;
  std::int64_t created_at_ms;
  std::int64_t updated_at_ms;
};

class ChatConversationRepository {
 public:
  explicit ChatConversationRepository(db::MySqlConnection& connection);

  [[nodiscard]] ChatConversation create(std::uint64_t user_id, chat::ConversationMode mode,
                                        std::int64_t occurred_at_ms);
  [[nodiscard]] std::vector<ChatConversation> listOwned(std::uint64_t user_id,
                                                         chat::ConversationMode mode);
  [[nodiscard]] std::optional<ChatConversation> findOwned(std::uint64_t user_id,
                                                           std::uint64_t conversation_id);
  [[nodiscard]] bool updateTitleIfDefault(std::uint64_t user_id, std::uint64_t conversation_id,
                                           const std::string& title, std::int64_t updated_at_ms);
  [[nodiscard]] bool removeOwned(std::uint64_t user_id, std::uint64_t conversation_id);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::persistence
