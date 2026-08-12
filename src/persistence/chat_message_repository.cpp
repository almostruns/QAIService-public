#include "persistence/chat_message_repository.h"

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

chat::ChatRole parseRole(const std::string& role)
{
  const std::optional<chat::ChatRole> parsed = chat::parseChatRole(role);
  if (!parsed.has_value()) {
    throw db::DatabaseError("stored chat message has an invalid role");
  }
  return parsed.value();
}

chat::ConversationMode parseMode(const std::string& mode)
{
  const std::optional<chat::ConversationMode> parsed = chat::parseConversationMode(mode);
  if (!parsed.has_value()) {
    throw db::DatabaseError("stored chat message has an invalid conversation mode");
  }
  return parsed.value();
}

std::uint64_t legacyConversationId(MYSQL* connection, std::uint64_t user_id, chat::ConversationMode mode)
{
  constexpr char query[] =
      "SELECT MIN(id) FROM chat_conversations WHERE user_id = ? AND conversation_mode = ?";
  db::PreparedStatement statement(connection, query);
  std::string mode_name = chat::conversationModeName(mode);
  unsigned long mode_length = mode_name.size();
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_STRING;
  parameters[1].buffer = mode_name.data();
  parameters[1].buffer_length = mode_name.size();
  parameters[1].length = &mode_length;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot execute legacy conversation query");
  }
  std::uint64_t conversation_id = 0;
  my_bool is_null = false;
  MYSQL_BIND result{};
  result.buffer_type = MYSQL_TYPE_LONGLONG;
  result.buffer = &conversation_id;
  result.is_unsigned = true;
  result.is_null = &is_null;
  if (mysql_stmt_bind_result(statement.get(), &result) != 0 || mysql_stmt_fetch(statement.get()) != 0 || is_null) {
    throw db::DatabaseError("legacy chat conversation is missing");
  }
  return conversation_id;
}

}  // namespace

ChatMessageRepository::ChatMessageRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

bool ChatMessageRepository::insertIdempotent(const ChatMessageCreated& event)
{
  MYSQL* connection = connection_.nativeHandle();
  const std::uint64_t resolved_conversation_id =
      event.conversation_id == 0 ? legacyConversationId(connection, event.user_id, event.mode) : event.conversation_id;
  db::executeQuery(connection, "START TRANSACTION", "cannot start chat message transaction");

  try {
    constexpr char query[] =
        "INSERT INTO chat_messages "
        "(event_id, user_id, conversation_mode, conversation_id, message_sequence, role, content, occurred_at_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE content = chat_messages.content";
    db::PreparedStatement statement(connection, query);
    std::string role = chat::chatRoleName(event.role);
    std::string mode = chat::conversationModeName(event.mode);
    unsigned long event_id_length = event.event_id.size();
    unsigned long mode_length = mode.size();
    unsigned long role_length = role.size();
    unsigned long content_length = event.content.size();
    std::uint64_t user_id = event.user_id;
    std::uint64_t conversation_id = resolved_conversation_id;
    std::uint64_t sequence = event.sequence;
    std::int64_t occurred_at_ms = event.occurred_at_ms;

    std::array<MYSQL_BIND, 8> parameters{};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = const_cast<char*>(event.event_id.data());
    parameters[0].buffer_length = event.event_id.size();
    parameters[0].length = &event_id_length;
    parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[1].buffer = &user_id;
    parameters[1].is_unsigned = true;
    parameters[2].buffer_type = MYSQL_TYPE_STRING;
    parameters[2].buffer = mode.data();
    parameters[2].buffer_length = mode.size();
    parameters[2].length = &mode_length;
    parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[3].buffer = &conversation_id;
    parameters[3].is_unsigned = true;
    parameters[4].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[4].buffer = &sequence;
    parameters[4].is_unsigned = true;
    parameters[5].buffer_type = MYSQL_TYPE_STRING;
    parameters[5].buffer = role.data();
    parameters[5].buffer_length = role.size();
    parameters[5].length = &role_length;
    parameters[6].buffer_type = MYSQL_TYPE_STRING;
    parameters[6].buffer = const_cast<char*>(event.content.data());
    parameters[6].buffer_length = event.content.size();
    parameters[6].length = &content_length;
    parameters[7].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[7].buffer = &occurred_at_ms;

    if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 ||
        mysql_stmt_execute(statement.get()) != 0) {
      throw db::DatabaseError("cannot insert chat message");
    }
    const my_ulonglong affected_rows = mysql_stmt_affected_rows(statement.get());
    db::executeQuery(connection, "COMMIT", "cannot commit chat message transaction");
    return affected_rows == 1;
  } catch (...) {
    mysql_query(connection, "ROLLBACK");
    throw;
  }
}

std::size_t ChatMessageRepository::countByEventId(const std::string& event_id)
{
  constexpr char query[] = "SELECT COUNT(*) FROM chat_messages WHERE event_id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), query);

  unsigned long event_id_length = event_id.size();
  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_STRING;
  parameter.buffer = const_cast<char*>(event_id.data());
  parameter.buffer_length = event_id.size();
  parameter.length = &event_id_length;
  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot count chat message");
  }

  std::uint64_t count = 0;
  MYSQL_BIND result{};
  result.buffer_type = MYSQL_TYPE_LONGLONG;
  result.buffer = &count;
  result.is_unsigned = true;
  if (mysql_stmt_bind_result(statement.get(), &result) != 0 || mysql_stmt_fetch(statement.get()) != 0) {
    throw db::DatabaseError("cannot fetch chat message count");
  }
  return static_cast<std::size_t>(count);
}

std::vector<ChatMessageCreated> ChatMessageRepository::findByUserOrdered(std::uint64_t user_id)
{
  return findByUserOrdered(user_id, chat::ConversationMode::kGeneral);
}

std::vector<ChatMessageCreated> ChatMessageRepository::findByUserOrdered(std::uint64_t user_id,
                                                                         chat::ConversationMode mode)
{
  constexpr char query[] =
      "SELECT event_id, message_sequence, role, content, occurred_at_ms, conversation_mode, conversation_id "
      "FROM chat_messages WHERE user_id = ? AND conversation_mode = ? "
      "ORDER BY conversation_id ASC, message_sequence ASC";
  db::PreparedStatement statement(connection_.nativeHandle(), query);
  std::string mode_name = chat::conversationModeName(mode);
  unsigned long mode_length = mode_name.size();
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_STRING;
  parameters[1].buffer = mode_name.data();
  parameters[1].buffer_length = mode_name.size();
  parameters[1].length = &mode_length;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot execute ordered chat history query");
  }

  std::array<char, 33> event_id{};
  std::uint64_t sequence = 0;
  std::array<char, 10> role{};
  std::array<char, 65536> content{};
  std::array<char, 8> stored_mode{};
  std::int64_t occurred_at_ms = 0;
  std::uint64_t conversation_id = 0;
  unsigned long event_id_length = 0;
  unsigned long role_length = 0;
  unsigned long content_length = 0;
  unsigned long stored_mode_length = 0;
  std::array<MYSQL_BIND, 7> results{};
  results[0].buffer_type = MYSQL_TYPE_STRING;
  results[0].buffer = event_id.data();
  results[0].buffer_length = event_id.size();
  results[0].length = &event_id_length;
  results[1].buffer_type = MYSQL_TYPE_LONGLONG;
  results[1].buffer = &sequence;
  results[1].is_unsigned = true;
  results[2].buffer_type = MYSQL_TYPE_STRING;
  results[2].buffer = role.data();
  results[2].buffer_length = role.size();
  results[2].length = &role_length;
  results[3].buffer_type = MYSQL_TYPE_STRING;
  results[3].buffer = content.data();
  results[3].buffer_length = content.size();
  results[3].length = &content_length;
  results[4].buffer_type = MYSQL_TYPE_LONGLONG;
  results[4].buffer = &occurred_at_ms;
  results[5].buffer_type = MYSQL_TYPE_STRING;
  results[5].buffer = stored_mode.data();
  results[5].buffer_length = stored_mode.size();
  results[5].length = &stored_mode_length;
  results[6].buffer_type = MYSQL_TYPE_LONGLONG;
  results[6].buffer = &conversation_id;
  results[6].is_unsigned = true;
  if (mysql_stmt_bind_result(statement.get(), results.data()) != 0) {
    throw db::DatabaseError("cannot bind ordered chat history results");
  }

  std::vector<ChatMessageCreated> messages;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0) {
      throw db::DatabaseError("cannot fetch ordered chat history");
    }

    const std::string stored_role(role.data(), role_length);
    const std::string stored_mode_name(stored_mode.data(), stored_mode_length);
    messages.push_back({std::string(event_id.data(), event_id_length), user_id, sequence, parseRole(stored_role),
                        std::string(content.data(), content_length), occurred_at_ms, parseMode(stored_mode_name),
                        conversation_id});
  }
  return messages;
}

std::vector<ChatMessageCreated> ChatMessageRepository::findByConversationOrdered(
    std::uint64_t user_id, std::uint64_t conversation_id)
{
  constexpr char query[] =
      "SELECT event_id, message_sequence, role, content, occurred_at_ms, conversation_mode "
      "FROM chat_messages WHERE user_id = ? AND conversation_id = ? ORDER BY message_sequence ASC";
  db::PreparedStatement statement(connection_.nativeHandle(), query);
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &conversation_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot execute conversation chat history query");
  }

  std::array<char, 33> event_id{};
  std::uint64_t sequence = 0;
  std::array<char, 10> role{};
  std::array<char, 65536> content{};
  std::array<char, 8> stored_mode{};
  std::int64_t occurred_at_ms = 0;
  unsigned long event_id_length = 0;
  unsigned long role_length = 0;
  unsigned long content_length = 0;
  unsigned long stored_mode_length = 0;
  std::array<MYSQL_BIND, 6> results{};
  results[0].buffer_type = MYSQL_TYPE_STRING;
  results[0].buffer = event_id.data();
  results[0].buffer_length = event_id.size();
  results[0].length = &event_id_length;
  results[1].buffer_type = MYSQL_TYPE_LONGLONG;
  results[1].buffer = &sequence;
  results[1].is_unsigned = true;
  results[2].buffer_type = MYSQL_TYPE_STRING;
  results[2].buffer = role.data();
  results[2].buffer_length = role.size();
  results[2].length = &role_length;
  results[3].buffer_type = MYSQL_TYPE_STRING;
  results[3].buffer = content.data();
  results[3].buffer_length = content.size();
  results[3].length = &content_length;
  results[4].buffer_type = MYSQL_TYPE_LONGLONG;
  results[4].buffer = &occurred_at_ms;
  results[5].buffer_type = MYSQL_TYPE_STRING;
  results[5].buffer = stored_mode.data();
  results[5].buffer_length = stored_mode.size();
  results[5].length = &stored_mode_length;
  if (mysql_stmt_bind_result(statement.get(), results.data()) != 0) {
    throw db::DatabaseError("cannot bind conversation chat history results");
  }

  std::vector<ChatMessageCreated> messages;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0) {
      throw db::DatabaseError("cannot fetch conversation chat history");
    }
    const std::string stored_role(role.data(), role_length);
    const std::string stored_mode_name(stored_mode.data(), stored_mode_length);
    messages.push_back({std::string(event_id.data(), event_id_length), user_id, sequence, parseRole(stored_role),
                        std::string(content.data(), content_length), occurred_at_ms, parseMode(stored_mode_name),
                        conversation_id});
  }
  return messages;
}

}  // namespace qaiservice::persistence
