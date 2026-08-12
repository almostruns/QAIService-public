#pragma once

#include "assistant/tool_command.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace qaiservice::assistant {

class ProposalStore {
 public:
  using Clock = std::function<std::int64_t()>;

  ProposalStore(Clock clock, std::int64_t lifetime_ms);
  [[nodiscard]] std::string create(std::uint64_t user_id, ToolCommand command);
  [[nodiscard]] std::optional<ToolCommand> consume(std::uint64_t user_id, const std::string& token);

 private:
  void removeExpired(std::int64_t now_ms);
  void eraseExpiryEntry(std::int64_t expires_at_ms, const std::string& token);

  struct Proposal {
    std::uint64_t user_id;
    ToolCommand command;
    std::int64_t expires_at_ms;
  };

  Clock clock_;
  std::int64_t lifetime_ms_;
  std::mutex mutex_;
  std::unordered_map<std::string, Proposal> proposals_;
  std::multimap<std::int64_t, std::string> expiry_index_;
};

}  // namespace qaiservice::assistant
