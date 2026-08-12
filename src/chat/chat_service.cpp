#include "chat/chat_service.h"

#include "logging/log.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qaiservice::chat {
namespace {

struct PendingRequest {
  std::string prompt;
  std::string system_context;
  ChatService::CompletionCallback callback;
  ChatService::ProgressCallback progress_callback;
  ChatService::CompletionTransformer completion_transformer;
};

struct ConversationKey {
  std::uint64_t user_id;
  ConversationMode mode;
  std::uint64_t conversation_id;

  bool operator==(const ConversationKey& other) const
  {
    return user_id == other.user_id && mode == other.mode && conversation_id == other.conversation_id;
  }
};

struct ConversationKeyHash {
  std::size_t operator()(const ConversationKey& key) const
  {
    const std::size_t user_hash = std::hash<std::uint64_t>{}(key.user_id);
    const std::size_t mode_hash = std::hash<int>{}(static_cast<int>(key.mode));
    const std::size_t conversation_hash = std::hash<std::uint64_t>{}(key.conversation_id);
    return user_hash ^ (mode_hash << 1) ^ (conversation_hash << 2);
  }
};

struct UserConversation {
  std::vector<ChatMessage> history;
  std::deque<PendingRequest> pending;
  bool worker_scheduled{false};
  std::uint64_t next_sequence{1};
};

std::size_t historyBytes(const std::vector<ChatMessage>& history)
{
  std::size_t total = 0;
  for (const ChatMessage& message : history) {
    total += message.content.size();
  }
  return total;
}

void trimHistory(std::vector<ChatMessage>& history, std::size_t max_messages, std::size_t max_bytes)
{
  while (history.size() >= 2 && (history.size() > max_messages || historyBytes(history) > max_bytes)) {
    history.erase(history.begin(), history.begin() + 2);
  }
}

}  // namespace

struct ChatService::State {
  State(std::unique_ptr<ChatModelProvider> owned_provider, TaskExecutor owned_executor,
        std::size_t pending_limit, std::size_t history_message_limit, std::size_t history_byte_limit,
        PersistenceSink sink)
      : provider(std::move(owned_provider)), executor(std::move(owned_executor)),
        max_pending_requests(pending_limit), max_history_messages(history_message_limit),
        max_history_bytes(history_byte_limit), persistence_sink(std::move(sink))
  {
  }

  std::unique_ptr<ChatModelProvider> provider;
  TaskExecutor executor;
  const std::size_t max_pending_requests;
  const std::size_t max_history_messages;
  const std::size_t max_history_bytes;
  PersistenceSink persistence_sink;
  std::mutex mutex;
  std::unordered_map<ConversationKey, UserConversation, ConversationKeyHash> conversations;
  std::size_t pending_requests{0};
};

ChatService::ChatService(std::unique_ptr<ChatModelProvider> provider, TaskExecutor executor,
                         std::size_t max_pending_requests, std::size_t max_history_messages,
                         std::size_t max_history_bytes, PersistenceSink persistence_sink)
{
  if (provider == nullptr || !executor || max_pending_requests == 0 || max_history_messages < 2 ||
      max_history_bytes == 0) {
    throw std::invalid_argument("invalid chat service configuration");
  }
  state_ = std::make_unique<State>(std::move(provider), std::move(executor), max_pending_requests,
                                   max_history_messages, max_history_bytes, std::move(persistence_sink));
}

ChatService::~ChatService() = default;

ChatSubmitStatus ChatService::submit(std::uint64_t user_id, std::string prompt, CompletionCallback callback)
{
  return submit(user_id, ConversationMode::kGeneral, std::move(prompt), "", std::move(callback));
}

ChatSubmitStatus ChatService::submit(std::uint64_t user_id, ConversationMode mode, std::string prompt,
                                     std::string system_context, CompletionCallback callback)
{
  return submit(user_id, mode, 0, std::move(prompt), std::move(system_context), std::move(callback));
}

ChatSubmitStatus ChatService::submit(std::uint64_t user_id, ConversationMode mode,
                                     std::uint64_t conversation_id, std::string prompt,
                                     std::string system_context, CompletionCallback callback,
                                     ProgressCallback progress_callback,
                                     CompletionTransformer completion_transformer)
{
  if (prompt.empty()) {
    return ChatSubmitStatus::kInvalidPrompt;
  }

  bool should_schedule = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pending_requests >= state_->max_pending_requests) {
      return ChatSubmitStatus::kBusy;
    }

    const ConversationKey key{user_id, mode, conversation_id};
    UserConversation& conversation = state_->conversations[key];
    conversation.pending.push_back(
        {std::move(prompt), std::move(system_context), std::move(callback), std::move(progress_callback),
         std::move(completion_transformer)});
    ++state_->pending_requests;
    if (!conversation.worker_scheduled) {
      conversation.worker_scheduled = true;
      should_schedule = true;
    }
  }

  if (should_schedule) {
    try {
      schedule(user_id, mode, conversation_id);
    } catch (const std::exception& error) {
      QAI_LOG(warn, "chat") << "chat_schedule_exception user_id=" << user_id << " error=" << error.what();
      std::lock_guard<std::mutex> lock(state_->mutex);
      const ConversationKey key{user_id, mode, conversation_id};
      UserConversation& conversation = state_->conversations[key];
      conversation.pending.pop_front();
      conversation.worker_scheduled = false;
      --state_->pending_requests;
      return ChatSubmitStatus::kBusy;
    }
  }
  return ChatSubmitStatus::kAccepted;
}

bool ChatService::restoreIfUninitialized(std::uint64_t user_id, std::vector<ChatMessage> history,
                                         std::uint64_t next_sequence)
{
  return restoreIfUninitialized(user_id, ConversationMode::kGeneral, std::move(history), next_sequence);
}

bool ChatService::restoreIfUninitialized(std::uint64_t user_id, ConversationMode mode,
                                         std::vector<ChatMessage> history, std::uint64_t next_sequence)
{
  return restoreIfUninitialized(user_id, mode, 0, std::move(history), next_sequence);
}

bool ChatService::restoreIfUninitialized(std::uint64_t user_id, ConversationMode mode,
                                         std::uint64_t conversation_id, std::vector<ChatMessage> history,
                                         std::uint64_t next_sequence)
{
  if (user_id == 0 || next_sequence == 0) {
    return false;
  }

  trimHistory(history, state_->max_history_messages, state_->max_history_bytes);
  std::lock_guard<std::mutex> lock(state_->mutex);
  const ConversationKey key{user_id, mode, conversation_id};
  if (state_->conversations.find(key) != state_->conversations.end()) {
    return false;
  }

  UserConversation conversation;
  conversation.history = std::move(history);
  conversation.next_sequence = next_sequence;
  state_->conversations.emplace(key, std::move(conversation));
  return true;
}

std::vector<ChatMessage> ChatService::history(std::uint64_t user_id) const
{
  return history(user_id, ConversationMode::kGeneral);
}

std::vector<ChatMessage> ChatService::history(std::uint64_t user_id, ConversationMode mode) const
{
  return history(user_id, mode, 0);
}

std::vector<ChatMessage> ChatService::history(std::uint64_t user_id, ConversationMode mode,
                                              std::uint64_t conversation_id) const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  const ConversationKey key{user_id, mode, conversation_id};
  const auto conversation = state_->conversations.find(key);
  if (conversation == state_->conversations.end()) {
    return {};
  }
  return conversation->second.history;
}

ConversationEraseStatus ChatService::eraseIfIdle(std::uint64_t user_id, ConversationMode mode,
                                                  std::uint64_t conversation_id)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  const ConversationKey key{user_id, mode, conversation_id};
  const auto conversation = state_->conversations.find(key);
  if (conversation == state_->conversations.end()) {
    return ConversationEraseStatus::kErased;
  }
  if (conversation->second.worker_scheduled || !conversation->second.pending.empty()) {
    return ConversationEraseStatus::kBusy;
  }
  state_->conversations.erase(conversation);
  return ConversationEraseStatus::kErased;
}

void ChatService::schedule(std::uint64_t user_id, ConversationMode mode, std::uint64_t conversation_id)
{
  Task task = [this, user_id, mode, conversation_id] {
    processOne(user_id, mode, conversation_id);
  };
  state_->executor(std::move(task));
}

void ChatService::processOne(std::uint64_t user_id, ConversationMode mode, std::uint64_t conversation_id)
{
  PendingRequest request;
  std::vector<ChatMessage> messages;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const ConversationKey key{user_id, mode, conversation_id};
    UserConversation& conversation = state_->conversations.at(key);
    request = std::move(conversation.pending.front());
    conversation.pending.pop_front();
    messages = conversation.history;
    if (!request.system_context.empty()) {
      messages.insert(messages.begin(), {ChatRole::kSystem, request.system_context});
    }
    messages.push_back({ChatRole::kUser, request.prompt});
  }

  ChatCompletion completion{ChatCompletionStatus::kUnavailable};
  try {
    if (request.progress_callback) {
      request.progress_callback(ChatExecutionStage::kModel);
    }
    completion = state_->provider->complete(messages);
    if (completion.status == ChatCompletionStatus::kSuccess && request.completion_transformer) {
      completion = request.completion_transformer(std::move(completion));
    }
  } catch (const std::exception& error) {
    QAI_LOG(warn, "chat") << "chat_provider_exception user_id=" << user_id << " mode=" << static_cast<int>(mode)
             << " conversation_id=" << conversation_id << " error=" << error.what();
    completion = {ChatCompletionStatus::kUnavailable};
  }

  bool schedule_next = false;
  std::optional<CompletedChatTurn> completed_turn;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const ConversationKey key{user_id, mode, conversation_id};
    UserConversation& conversation = state_->conversations.at(key);
    if (completion.status == ChatCompletionStatus::kSuccess) {
      const ChatMessage user_message{ChatRole::kUser, request.prompt};
      const std::uint64_t first_sequence = conversation.next_sequence;
      conversation.next_sequence += 2;
      conversation.history.push_back({ChatRole::kUser, std::move(request.prompt)});
      conversation.history.push_back(completion.message);
      trimHistory(conversation.history, state_->max_history_messages, state_->max_history_bytes);
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const std::int64_t occurred_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
      completed_turn = CompletedChatTurn{user_id, mode, first_sequence, user_message, completion.message,
                                         occurred_at_ms, conversation_id};
    }

    --state_->pending_requests;
    schedule_next = !conversation.pending.empty();
    if (!schedule_next) {
      conversation.worker_scheduled = false;
    }
  }

  if (completed_turn.has_value() && state_->persistence_sink) {
    try {
      if (request.progress_callback) {
        request.progress_callback(ChatExecutionStage::kPersistence);
      }
      completion.persistence = state_->persistence_sink(std::move(completed_turn.value()));
    } catch (const std::exception& error) {
      QAI_LOG(warn, "chat") << "chat_persistence_sink_exception user_id=" << user_id << " error=" << error.what();
      completion.persistence = ChatPersistenceStatus::kUnavailable;
    }
  }

  try {
    request.callback(std::move(completion));
  } catch (const std::exception& error) {
    QAI_LOG(warn, "chat") << "chat_callback_exception user_id=" << user_id << " error=" << error.what();
    // 响应发送失败只影响当前请求，不能破坏同一用户队列的推进。
  }
  if (schedule_next) {
    schedule(user_id, mode, conversation_id);
  }
}

}  // namespace qaiservice::chat
