#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qaiservice::chat {

enum class ChatProgressStatus {
  kRunning,
  kCompleted,
  kFailed,
};

struct ChatProgressStage {
  std::string code;
  std::string label;
};

enum class ChatProgressStageStatus {
  kPending,
  kRunning,
  kCompleted,
  kSkipped,
  kFailed,
};

struct ChatProgressStageSnapshot {
  std::string code;
  std::string label;
  ChatProgressStageStatus status;
};

struct ChatProgressSnapshot {
  std::uint64_t conversation_id;
  ChatProgressStatus status;
  std::size_t stage_index;
  std::size_t stage_count;
  ChatProgressStage stage;
  std::vector<ChatProgressStageSnapshot> stages;
  std::int64_t updated_at_ms;
};

struct ChatProgressLookup {
  std::string request_id;
  ChatProgressSnapshot progress;
};

class ChatProgressStore {
 public:
  using NowFunction = std::function<std::int64_t()>;

  ChatProgressStore(NowFunction now, std::chrono::seconds ttl);

  [[nodiscard]] bool create(std::uint64_t user_id, std::uint64_t conversation_id, std::string request_id,
                            std::vector<ChatProgressStage> stages);
  [[nodiscard]] bool advance(std::uint64_t user_id, const std::string& request_id, std::size_t stage_index);
  [[nodiscard]] bool complete(std::uint64_t user_id, const std::string& request_id);
  [[nodiscard]] bool fail(std::uint64_t user_id, const std::string& request_id);
  [[nodiscard]] std::optional<ChatProgressSnapshot> findOwned(std::uint64_t user_id,
                                                              const std::string& request_id);
  [[nodiscard]] std::optional<ChatProgressLookup> findLatestByConversation(std::uint64_t user_id,
                                                                           std::uint64_t conversation_id);
  [[nodiscard]] std::size_t size();

 private:
  struct Entry {
    std::uint64_t user_id;
    std::uint64_t conversation_id;
    ChatProgressStatus status;
    std::size_t stage_index;
    std::vector<ChatProgressStage> stages;
    std::vector<ChatProgressStageStatus> stage_statuses;
    std::int64_t updated_at_ms;
    std::uint64_t generation;
  };

  void removeExpired(std::int64_t now_ms);
  [[nodiscard]] static ChatProgressSnapshot snapshot(const Entry& entry);
  [[nodiscard]] static bool validStages(const std::vector<ChatProgressStage>& stages);

  NowFunction now_;
  std::int64_t ttl_ms_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t next_generation_{1};
};

}  // namespace qaiservice::chat
