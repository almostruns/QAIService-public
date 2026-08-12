#include "web_search/search_consent_store.h"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace qaiservice::web_search {

SearchConsentStore::SearchConsentStore(Clock clock, std::int64_t lifetime_ms, std::size_t capacity)
    : clock_(std::move(clock)), lifetime_ms_(lifetime_ms), capacity_(capacity)
{
  if (!clock_ || lifetime_ms_ <= 0 || capacity_ == 0 || sodium_init() < 0) {
    throw std::invalid_argument("invalid search consent store configuration");
  }
}

std::string SearchConsentStore::create(std::uint64_t user_id, std::string mode, std::string conversation_id,
                                       std::string message_digest, bool required_for_answer,
                                       std::vector<std::string> queries,
                                       std::vector<std::string> risk_types)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::int64_t now_ms = clock_();
  removeExpired(now_ms);
  enforceCapacity();

  std::string token = generateToken();
  while (entries_.count(token) != 0) {
    token = generateToken();
  }

  SearchConsent consent{required_for_answer, std::move(queries), std::move(risk_types)};
  Entry entry{user_id, std::move(mode), std::move(conversation_id), std::move(message_digest), std::move(consent),
              now_ms + lifetime_ms_};
  entries_.emplace(token, std::move(entry));
  insertion_order_.push_back(token);
  return token;
}

std::optional<SearchConsent> SearchConsentStore::consumeOwned(const std::string& token, std::uint64_t user_id,
                                                              const std::string& mode,
                                                              const std::string& conversation_id,
                                                              const std::string& message_digest)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::int64_t now_ms = clock_();
  removeExpired(now_ms);

  const auto entry = entries_.find(token);
  if (entry == entries_.end()) {
    return std::nullopt;
  }
  const bool owner_matches = entry->second.user_id == user_id && entry->second.mode == mode &&
                             entry->second.conversation_id == conversation_id &&
                             entry->second.message_digest == message_digest;
  if (!owner_matches) {
    return std::nullopt;
  }

  SearchConsent consent = std::move(entry->second.consent);
  entries_.erase(entry);
  return consent;
}

std::string SearchConsentStore::generateToken() const
{
  std::array<unsigned char, 24> random{};
  randombytes_buf(random.data(), random.size());
  std::array<char, 49> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), random.data(), random.size());
  return std::string(encoded.data());
}

void SearchConsentStore::removeExpired(std::int64_t now_ms)
{
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (entry->second.expires_at_ms <= now_ms) {
      entry = entries_.erase(entry);
    } else {
      ++entry;
    }
  }
}

void SearchConsentStore::enforceCapacity()
{
  while (entries_.size() >= capacity_ && !insertion_order_.empty()) {
    const std::string oldest_token = insertion_order_.front();
    insertion_order_.pop_front();
    const std::size_t removed = entries_.erase(oldest_token);
    if (removed != 0) {
      break;
    }
  }
}

}  // namespace qaiservice::web_search
