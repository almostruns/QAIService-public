#pragma once

#include "persistence/chat_message_event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::persistence {

class ChatMessageRepository {
 public:
  explicit ChatMessageRepository(db::MySqlConnection& connection);

  [[nodiscard]] bool insertIdempotent(const ChatMessageCreated& event);
  [[nodiscard]] std::size_t countByEventId(const std::string& event_id);
  [[nodiscard]] std::vector<ChatMessageCreated> findByUserOrdered(std::uint64_t user_id);
  [[nodiscard]] std::vector<ChatMessageCreated> findByUserOrdered(std::uint64_t user_id,
                                                                  chat::ConversationMode mode);
  [[nodiscard]] std::vector<ChatMessageCreated> findByConversationOrdered(std::uint64_t user_id,
                                                                          std::uint64_t conversation_id);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::persistence
