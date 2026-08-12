#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qaiservice::web_search {

struct SearchConsent {
  bool required_for_answer{false};
  std::vector<std::string> queries;
  std::vector<std::string> risk_types;
};

class SearchConsentStore {
public:
  using Clock = std::function<std::int64_t()>;

  SearchConsentStore(Clock clock, std::int64_t lifetime_ms, std::size_t capacity);

  [[nodiscard]] std::string create(std::uint64_t user_id, std::string mode, std::string conversation_id,
                                   std::string message_digest, bool required_for_answer,
                                   std::vector<std::string> queries,
                                   std::vector<std::string> risk_types);
  [[nodiscard]] std::optional<SearchConsent> consumeOwned(const std::string& token, std::uint64_t user_id,
                                                          const std::string& mode,
                                                          const std::string& conversation_id,
                                                          const std::string& message_digest);

private:
  struct Entry {
    std::uint64_t user_id;
    std::string mode;
    std::string conversation_id;
    std::string message_digest;
    SearchConsent consent;
    std::int64_t expires_at_ms;
  };

  [[nodiscard]] std::string generateToken() const;
  void removeExpired(std::int64_t now_ms);
  void enforceCapacity();

  Clock clock_;
  std::int64_t lifetime_ms_;
  std::size_t capacity_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::deque<std::string> insertion_order_;
};

}  // namespace qaiservice::web_search
