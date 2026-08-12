#pragma once

#include "chat/chat_model_provider.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qaiservice::chat {

enum class ChatSubmitStatus {
  kAccepted,
  kInvalidPrompt,
  kBusy,
};

enum class ConversationEraseStatus {
  kErased,
  kBusy,
};

enum class ChatExecutionStage {
  kModel,
  kPersistence,
};

struct CompletedChatTurn {
  std::uint64_t user_id;
  ConversationMode mode;
  std::uint64_t first_sequence;
  ChatMessage user_message;
  ChatMessage assistant_message;
  std::int64_t occurred_at_ms;
  std::uint64_t conversation_id{0};
};

class ChatService {
 public:
  using Task = std::function<void()>;
  using TaskExecutor = std::function<void(Task)>;
  using CompletionCallback = std::function<void(ChatCompletion)>;
  using ProgressCallback = std::function<void(ChatExecutionStage)>;
  using CompletionTransformer = std::function<ChatCompletion(ChatCompletion)>;
  using PersistenceSink = std::function<ChatPersistenceStatus(CompletedChatTurn)>;

  ChatService(std::unique_ptr<ChatModelProvider> provider, TaskExecutor executor,
              std::size_t max_pending_requests = 64, std::size_t max_history_messages = 20,
              std::size_t max_history_bytes = 32768, PersistenceSink persistence_sink = {});
  ~ChatService();

  [[nodiscard]] ChatSubmitStatus submit(std::uint64_t user_id, std::string prompt,
                                        CompletionCallback callback);
  [[nodiscard]] ChatSubmitStatus submit(std::uint64_t user_id, ConversationMode mode, std::string prompt,
                                        std::string system_context, CompletionCallback callback);
  [[nodiscard]] ChatSubmitStatus submit(std::uint64_t user_id, ConversationMode mode,
                                        std::uint64_t conversation_id, std::string prompt,
                                        std::string system_context, CompletionCallback callback,
                                        ProgressCallback progress_callback = {},
                                        CompletionTransformer completion_transformer = {});
  [[nodiscard]] bool restoreIfUninitialized(std::uint64_t user_id, std::vector<ChatMessage> history,
                                            std::uint64_t next_sequence);
  [[nodiscard]] bool restoreIfUninitialized(std::uint64_t user_id, ConversationMode mode,
                                            std::vector<ChatMessage> history, std::uint64_t next_sequence);
  [[nodiscard]] bool restoreIfUninitialized(std::uint64_t user_id, ConversationMode mode,
                                            std::uint64_t conversation_id, std::vector<ChatMessage> history,
                                            std::uint64_t next_sequence);
  [[nodiscard]] std::vector<ChatMessage> history(std::uint64_t user_id) const;
  [[nodiscard]] std::vector<ChatMessage> history(std::uint64_t user_id, ConversationMode mode) const;
  [[nodiscard]] std::vector<ChatMessage> history(std::uint64_t user_id, ConversationMode mode,
                                                 std::uint64_t conversation_id) const;
  [[nodiscard]] ConversationEraseStatus eraseIfIdle(std::uint64_t user_id, ConversationMode mode,
                                                     std::uint64_t conversation_id);

 private:
  struct State;

  void schedule(std::uint64_t user_id, ConversationMode mode, std::uint64_t conversation_id);
  void processOne(std::uint64_t user_id, ConversationMode mode, std::uint64_t conversation_id);

  std::unique_ptr<State> state_;
};

}  // namespace qaiservice::chat
