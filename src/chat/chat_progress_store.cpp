#include "chat/chat_progress_store.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace qaiservice::chat {

ChatProgressStore::ChatProgressStore(NowFunction now, std::chrono::seconds ttl)
    : now_(std::move(now)), ttl_ms_(std::chrono::duration_cast<std::chrono::milliseconds>(ttl).count())
{
  if (!now_ || ttl_ms_ <= 0) {
    throw std::invalid_argument("invalid chat progress store configuration");
  }
}

bool ChatProgressStore::create(std::uint64_t user_id, std::uint64_t conversation_id, std::string request_id,
                               std::vector<ChatProgressStage> stages)
{
  if (user_id == 0 || conversation_id == 0 || request_id.empty() || request_id.size() > 128 ||
      !validStages(stages)) {
    return false;
  }

  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  const bool conversation_running = std::any_of(entries_.begin(), entries_.end(),
                                                [user_id, conversation_id](const auto& item) {
    return item.second.user_id == user_id && item.second.conversation_id == conversation_id &&
           item.second.status == ChatProgressStatus::kRunning;
  });
  if (conversation_running) {
    return false;
  }
  std::vector<ChatProgressStageStatus> stage_statuses(stages.size(), ChatProgressStageStatus::kPending);
  stage_statuses.front() = ChatProgressStageStatus::kRunning;
  Entry entry{user_id, conversation_id, ChatProgressStatus::kRunning, 0, std::move(stages),
              std::move(stage_statuses), now_ms, next_generation_++};
  return entries_.emplace(std::move(request_id), std::move(entry)).second;
}

bool ChatProgressStore::advance(std::uint64_t user_id, const std::string& request_id, std::size_t stage_index)
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  const auto found = entries_.find(request_id);
  if (found == entries_.end() || found->second.user_id != user_id ||
      found->second.status != ChatProgressStatus::kRunning || stage_index <= found->second.stage_index ||
      stage_index >= found->second.stages.size()) {
    return false;
  }
  found->second.stage_statuses[found->second.stage_index] = ChatProgressStageStatus::kCompleted;
  for (std::size_t index = found->second.stage_index + 1; index < stage_index; ++index) {
    found->second.stage_statuses[index] = ChatProgressStageStatus::kSkipped;
  }
  found->second.stage_statuses[stage_index] = ChatProgressStageStatus::kRunning;
  found->second.stage_index = stage_index;
  found->second.updated_at_ms = now_ms;
  return true;
}

bool ChatProgressStore::complete(std::uint64_t user_id, const std::string& request_id)
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  const auto found = entries_.find(request_id);
  if (found == entries_.end() || found->second.user_id != user_id ||
      found->second.status != ChatProgressStatus::kRunning) {
    return false;
  }
  const std::size_t final_stage_index = found->second.stages.size() - 1;
  found->second.stage_statuses[found->second.stage_index] = ChatProgressStageStatus::kCompleted;
  for (std::size_t index = found->second.stage_index + 1; index < final_stage_index; ++index) {
    found->second.stage_statuses[index] = ChatProgressStageStatus::kSkipped;
  }
  found->second.stage_statuses[final_stage_index] = ChatProgressStageStatus::kCompleted;
  found->second.stage_index = final_stage_index;
  found->second.status = ChatProgressStatus::kCompleted;
  found->second.updated_at_ms = now_ms;
  return true;
}

bool ChatProgressStore::fail(std::uint64_t user_id, const std::string& request_id)
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  const auto found = entries_.find(request_id);
  if (found == entries_.end() || found->second.user_id != user_id ||
      found->second.status != ChatProgressStatus::kRunning) {
    return false;
  }
  found->second.stage_statuses[found->second.stage_index] = ChatProgressStageStatus::kFailed;
  found->second.status = ChatProgressStatus::kFailed;
  found->second.updated_at_ms = now_ms;
  return true;
}

std::optional<ChatProgressSnapshot> ChatProgressStore::findOwned(std::uint64_t user_id,
                                                                 const std::string& request_id)
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  const auto found = entries_.find(request_id);
  if (found == entries_.end() || found->second.user_id != user_id) {
    return std::nullopt;
  }
  return snapshot(found->second);
}

std::optional<ChatProgressLookup> ChatProgressStore::findLatestByConversation(std::uint64_t user_id,
                                                                              std::uint64_t conversation_id)
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);

  const auto latest = std::max_element(entries_.begin(), entries_.end(),
                                       [user_id, conversation_id](const auto& left, const auto& right) {
    const bool left_matches = left.second.user_id == user_id && left.second.conversation_id == conversation_id;
    const bool right_matches = right.second.user_id == user_id && right.second.conversation_id == conversation_id;
    if (left_matches != right_matches) {
      return !left_matches;
    }
    if (left.second.updated_at_ms != right.second.updated_at_ms) {
      return left.second.updated_at_ms < right.second.updated_at_ms;
    }
    return left.second.generation < right.second.generation;
  });
  if (latest == entries_.end() || latest->second.user_id != user_id ||
      latest->second.conversation_id != conversation_id) {
    return std::nullopt;
  }
  return ChatProgressLookup{latest->first, snapshot(latest->second)};
}

std::size_t ChatProgressStore::size()
{
  const std::int64_t now_ms = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(now_ms);
  return entries_.size();
}

void ChatProgressStore::removeExpired(std::int64_t now_ms)
{
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (entry->second.updated_at_ms + ttl_ms_ <= now_ms) {
      entry = entries_.erase(entry);
    } else {
      ++entry;
    }
  }
}

bool ChatProgressStore::validStages(const std::vector<ChatProgressStage>& stages)
{
  if (stages.empty() || stages.size() > 16) {
    return false;
  }
  return std::all_of(stages.begin(), stages.end(), [](const ChatProgressStage& stage) {
    return !stage.code.empty() && stage.code.size() <= 64 && !stage.label.empty() && stage.label.size() <= 128;
  });
}

ChatProgressSnapshot ChatProgressStore::snapshot(const Entry& entry)
{
  std::vector<ChatProgressStageSnapshot> stages;
  stages.reserve(entry.stages.size());
  for (std::size_t index = 0; index < entry.stages.size(); ++index) {
    stages.push_back({entry.stages[index].code, entry.stages[index].label, entry.stage_statuses[index]});
  }
  return ChatProgressSnapshot{entry.conversation_id, entry.status, entry.stage_index, entry.stages.size(),
                              entry.stages[entry.stage_index], std::move(stages), entry.updated_at_ms};
}

}  // namespace qaiservice::chat
