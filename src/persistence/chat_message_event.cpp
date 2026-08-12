#include "persistence/chat_message_event.h"

#include "chat/chat_names.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>

namespace qaiservice::persistence {
namespace {

constexpr int kSchemaVersion = 2;
constexpr std::size_t kEventIdBytes = 16;
constexpr std::size_t kEventIdCharacters = kEventIdBytes * 2;

bool validEventId(const std::string& event_id)
{
  if (event_id.size() != kEventIdCharacters) {
    return false;
  }
  for (const unsigned char character : event_id) {
    if (!std::isxdigit(character)) {
      return false;
    }
  }
  return true;
}

bool validEvent(const ChatMessageCreated& event)
{
  return validEventId(event.event_id) && event.user_id > 0 && event.sequence > 0 &&
         !event.content.empty() && event.occurred_at_ms > 0;
}

}  // namespace

std::string makeEventId()
{
  static const int initialized = sodium_init();
  if (initialized < 0) {
    throw std::runtime_error("cannot initialize secure random generator");
  }

  std::array<unsigned char, kEventIdBytes> bytes{};
  std::array<char, kEventIdCharacters + 1> encoded{};
  randombytes_buf(bytes.data(), bytes.size());
  sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
  return encoded.data();
}

std::string chatMessageEventToJson(const ChatMessageCreated& event)
{
  if (!validEvent(event)) {
    throw std::invalid_argument("invalid chat message event");
  }

  const nlohmann::json body{{"schema_version", kSchemaVersion},
                            {"event_id", event.event_id},
                            {"user_id", event.user_id},
                            {"sequence", event.sequence},
                            {"role", chat::chatRoleName(event.role)},
                            {"content", event.content},
                            {"occurred_at_ms", event.occurred_at_ms},
                            {"mode", chat::conversationModeName(event.mode)},
                            {"conversation_id", event.conversation_id}};
  return body.dump();
}

std::optional<ChatMessageCreated> chatMessageEventFromJson(const std::string& json)
{
  try {
    const nlohmann::json body = nlohmann::json::parse(json);
    if (!body.is_object() || !body.contains("schema_version") || !body["schema_version"].is_number_integer() ||
        !body.contains("event_id") || !body["event_id"].is_string() || !body.contains("user_id") ||
        !body["user_id"].is_number_unsigned() || !body.contains("sequence") ||
        !body["sequence"].is_number_unsigned() || !body.contains("role") || !body["role"].is_string() ||
        !body.contains("content") || !body["content"].is_string() || !body.contains("occurred_at_ms") ||
        !body["occurred_at_ms"].is_number_integer()) {
      return std::nullopt;
    }
    const int schema_version = body["schema_version"].get<int>();
    if (schema_version != 1 && schema_version != kSchemaVersion) {
      return std::nullopt;
    }

    const std::optional<chat::ChatRole> role = chat::parseChatRole(body["role"].get<std::string>());
    if (!role.has_value()) {
      return std::nullopt;
    }

    chat::ConversationMode mode = chat::ConversationMode::kGeneral;
    if (body.contains("mode")) {
      if (!body["mode"].is_string()) {
        return std::nullopt;
      }
      const std::optional<chat::ConversationMode> parsed_mode =
          chat::parseConversationMode(body["mode"].get<std::string>());
      if (!parsed_mode.has_value()) {
        return std::nullopt;
      }
      mode = parsed_mode.value();
    }

    std::uint64_t conversation_id = 0;
    if (body.contains("conversation_id")) {
      if (!body["conversation_id"].is_number_unsigned()) {
        return std::nullopt;
      }
      conversation_id = body["conversation_id"].get<std::uint64_t>();
    }

    ChatMessageCreated event{body["event_id"].get<std::string>(), body["user_id"].get<std::uint64_t>(),
                             body["sequence"].get<std::uint64_t>(), role.value(),
                             body["content"].get<std::string>(), body["occurred_at_ms"].get<std::int64_t>(), mode,
                             conversation_id};
    if (!validEvent(event)) {
      return std::nullopt;
    }
    return event;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

}  // namespace qaiservice::persistence
