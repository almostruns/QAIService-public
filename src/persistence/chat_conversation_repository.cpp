#include "persistence/chat_conversation_repository.h"

#include "chat/chat_names.h"
#include "db/mysql_connection.h"
#include "db/mysql_statement.h"

#include <mysql.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace qaiservice::persistence {
namespace {

constexpr char kDefaultTitle[] = "新对话";

chat::ConversationMode parseMode(const std::string& mode)
{
  const std::optional<chat::ConversationMode> parsed = chat::parseConversationMode(mode);
  if (!parsed.has_value()) {
    throw db::DatabaseError("stored chat conversation has an invalid mode");
  }
  return parsed.value();
}

struct ConversationResult {
  std::uint64_t id{0};
  std::uint64_t user_id{0};
  std::array<char, 8> mode{};
  std::array<char, 641> title{};
  std::int64_t created_at_ms{0};
  std::int64_t updated_at_ms{0};
  unsigned long mode_length{0};
  unsigned long title_length{0};
  std::array<MYSQL_BIND, 6> bindings{};

  ConversationResult()
  {
    bindings[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[0].buffer = &id;
    bindings[0].is_unsigned = true;
    bindings[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[1].buffer = &user_id;
    bindings[1].is_unsigned = true;
    bindings[2].buffer_type = MYSQL_TYPE_STRING;
    bindings[2].buffer = mode.data();
    bindings[2].buffer_length = mode.size();
    bindings[2].length = &mode_length;
    bindings[3].buffer_type = MYSQL_TYPE_STRING;
    bindings[3].buffer = title.data();
    bindings[3].buffer_length = title.size();
    bindings[3].length = &title_length;
    bindings[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[4].buffer = &created_at_ms;
    bindings[5].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[5].buffer = &updated_at_ms;
  }

  [[nodiscard]] ChatConversation value() const
  {
    return {id, user_id, parseMode(std::string(mode.data(), mode_length)),
            std::string(title.data(), title_length), created_at_ms, updated_at_ms};
  }
};

}  // namespace

ChatConversationRepository::ChatConversationRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

ChatConversation ChatConversationRepository::create(std::uint64_t user_id, chat::ConversationMode mode,
                                                      std::int64_t occurred_at_ms)
{
  constexpr char sql[] =
      "INSERT INTO chat_conversations (user_id, conversation_mode, title, created_at_ms, updated_at_ms) "
      "VALUES (?, ?, ?, ?, ?)";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::string mode_name = chat::conversationModeName(mode);
  const std::string title = kDefaultTitle;
  unsigned long mode_length = 0;
  unsigned long title_length = 0;
  std::array<MYSQL_BIND, 5> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  db::bindString(parameters[1], mode_name, mode_length);
  db::bindString(parameters[2], title, title_length);
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &occurred_at_ms;
  parameters[4].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[4].buffer = &occurred_at_ms;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot create chat conversation");
  }
  const std::uint64_t conversation_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(statement.get()));
  const std::optional<ChatConversation> conversation = findOwned(user_id, conversation_id);
  if (!conversation.has_value()) {
    throw db::DatabaseError("created chat conversation is missing");
  }
  return conversation.value();
}

std::vector<ChatConversation> ChatConversationRepository::listOwned(std::uint64_t user_id,
                                                                     chat::ConversationMode mode)
{
  constexpr char sql[] =
      "SELECT id, user_id, conversation_mode, title, created_at_ms, updated_at_ms FROM chat_conversations "
      "WHERE user_id = ? AND conversation_mode = ? ORDER BY updated_at_ms DESC, id DESC";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::string mode_name = chat::conversationModeName(mode);
  unsigned long mode_length = 0;
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  db::bindString(parameters[1], mode_name, mode_length);
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot list chat conversations");
  }
  ConversationResult result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind chat conversation list");
  }
  std::vector<ChatConversation> conversations;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0) {
      throw db::DatabaseError("cannot fetch chat conversation list");
    }
    conversations.push_back(result.value());
  }
  return conversations;
}

std::optional<ChatConversation> ChatConversationRepository::findOwned(std::uint64_t user_id,
                                                                       std::uint64_t conversation_id)
{
  constexpr char sql[] =
      "SELECT id, user_id, conversation_mode, title, created_at_ms, updated_at_ms FROM chat_conversations "
      "WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &conversation_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot find owned chat conversation");
  }
  ConversationResult result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind owned chat conversation");
  }
  const int fetched = mysql_stmt_fetch(statement.get());
  if (fetched == MYSQL_NO_DATA) {
    return std::nullopt;
  }
  if (fetched != 0) {
    throw db::DatabaseError("cannot fetch owned chat conversation");
  }
  return result.value();
}

bool ChatConversationRepository::updateTitleIfDefault(std::uint64_t user_id, std::uint64_t conversation_id,
                                                       const std::string& title, std::int64_t updated_at_ms)
{
  constexpr char sql[] =
      "UPDATE chat_conversations SET title = ?, updated_at_ms = ? "
      "WHERE user_id = ? AND id = ? AND title = '新对话'";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  unsigned long title_length = 0;
  std::array<MYSQL_BIND, 4> parameters{};
  db::bindString(parameters[0], title, title_length);
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &updated_at_ms;
  parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[2].buffer = &user_id;
  parameters[2].is_unsigned = true;
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &conversation_id;
  parameters[3].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot update chat conversation title");
  }
  return mysql_stmt_affected_rows(statement.get()) == 1;
}

bool ChatConversationRepository::removeOwned(std::uint64_t user_id, std::uint64_t conversation_id)
{
  constexpr char sql[] = "DELETE FROM chat_conversations WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &conversation_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot remove chat conversation");
  }
  return mysql_stmt_affected_rows(statement.get()) == 1;
}

}  // namespace qaiservice::persistence
