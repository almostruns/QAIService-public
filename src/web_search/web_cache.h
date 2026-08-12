#pragma once

#include "web_search/search_provider.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace qaiservice::web_search {
namespace detail {

template <typename Value>
class LruTtlCache {
public:
  using Clock = std::function<std::int64_t()>;

  LruTtlCache(Clock clock, std::int64_t lifetime_ms, std::size_t capacity)
      : clock_(std::move(clock)), lifetime_ms_(lifetime_ms), capacity_(capacity)
  {
  }

  std::optional<Value> get(const std::string& key)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
      return std::nullopt;
    }
    if (entry->second.expires_at_ms <= clock_()) {
      order_.erase(entry->second.order);
      entries_.erase(entry);
      return std::nullopt;
    }
    order_.splice(order_.end(), order_, entry->second.order);
    return entry->second.value;
  }

  void put(const std::string& key, Value value)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ || lifetime_ms_ <= 0 || capacity_ == 0 || key.empty()) {
      return;
    }
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      order_.erase(existing->second.order);
      entries_.erase(existing);
    }
    while (entries_.size() >= capacity_) {
      entries_.erase(order_.front());
      order_.pop_front();
    }
    order_.push_back(key);
    const auto order = std::prev(order_.end());
    entries_.emplace(key, Entry{std::move(value), clock_() + lifetime_ms_, order});
  }

private:
  struct Entry {
    Value value;
    std::int64_t expires_at_ms;
    typename std::list<std::string>::iterator order;
  };

  Clock clock_;
  std::int64_t lifetime_ms_;
  std::size_t capacity_;
  std::mutex mutex_;
  std::list<std::string> order_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace detail

class SearchResponseCache {
public:
  using Clock = std::function<std::int64_t()>;

  SearchResponseCache(Clock clock, std::int64_t lifetime_ms, std::size_t capacity);
  [[nodiscard]] std::optional<SearchResponse> get(const std::string& query);
  void put(const std::string& query, SearchResponse response, bool sensitive);

private:
  detail::LruTtlCache<SearchResponse> cache_;
};

class PageBodyCache {
public:
  using Clock = std::function<std::int64_t()>;

  PageBodyCache(Clock clock, std::int64_t lifetime_ms, std::size_t capacity);
  [[nodiscard]] std::optional<std::string> get(const std::string& url);
  void put(const std::string& url, std::string body);

private:
  detail::LruTtlCache<std::string> cache_;
};

}  // namespace qaiservice::web_search
