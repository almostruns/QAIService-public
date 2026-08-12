#include "assistant/proposal_store.h"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace qaiservice::assistant {

ProposalStore::ProposalStore(Clock clock, std::int64_t lifetime_ms)
    : clock_(std::move(clock)), lifetime_ms_(lifetime_ms)
{
  if (!clock_ || lifetime_ms_ <= 0 || sodium_init() < 0) {
    throw std::invalid_argument("invalid proposal store configuration");
  }
}

std::string ProposalStore::create(std::uint64_t user_id, ToolCommand command)
{
  std::array<unsigned char, 24> random{};
  randombytes_buf(random.data(), random.size());
  std::array<char, 49> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), random.data(), random.size());
  const std::string token(encoded.data());
  std::lock_guard<std::mutex> lock(mutex_);
  removeExpired(clock_());
  proposals_.emplace(token, Proposal{user_id, std::move(command), clock_() + lifetime_ms_});
  expiry_index_.emplace(proposals_.at(token).expires_at_ms, token);
  return token;
}

std::optional<ToolCommand> ProposalStore::consume(std::uint64_t user_id, const std::string& token)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto proposal = proposals_.find(token);
  if (proposal == proposals_.end() || proposal->second.user_id != user_id) {
    return std::nullopt;
  }
  if (proposal->second.expires_at_ms < clock_()) {
    const std::int64_t expires_at_ms = proposal->second.expires_at_ms;
    proposals_.erase(proposal);
    eraseExpiryEntry(expires_at_ms, token);
    return std::nullopt;
  }
  ToolCommand command = std::move(proposal->second.command);
  const std::int64_t expires_at_ms = proposal->second.expires_at_ms;
  proposals_.erase(proposal);
  eraseExpiryEntry(expires_at_ms, token);
  return command;
}

void ProposalStore::eraseExpiryEntry(std::int64_t expires_at_ms, const std::string& token)
{
  const auto range = expiry_index_.equal_range(expires_at_ms);
  for (auto entry = range.first; entry != range.second; ++entry) {
    if (entry->second == token) {
      expiry_index_.erase(entry);
      break;
    }
  }
}

void ProposalStore::removeExpired(std::int64_t now_ms)
{
  while (!expiry_index_.empty() && expiry_index_.begin()->first <= now_ms) {
    const std::string token = expiry_index_.begin()->second;
    expiry_index_.erase(expiry_index_.begin());
    proposals_.erase(token);
  }
}

}  // namespace qaiservice::assistant
