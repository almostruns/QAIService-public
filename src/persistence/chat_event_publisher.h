#pragma once

#include "persistence/chat_message_event.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace qaiservice::persistence {

enum class TransportPublishResult {
  kConfirmed,
  kRetryableFailure,
  kPermanentFailure,
};

class ChatEventTransport {
 public:
  virtual ~ChatEventTransport() = default;

  [[nodiscard]] virtual TransportPublishResult publish(const ChatMessageCreated& event) = 0;
};

enum class EnqueueStatus {
  kQueued,
  kBusy,
  kUnavailable,
};

struct PublisherSnapshot {
  std::size_t pending{0};
  std::size_t confirmed{0};
  std::size_t failed{0};
};

class ChatEventPublisher {
 public:
  ChatEventPublisher(std::unique_ptr<ChatEventTransport> transport, std::size_t capacity = 128,
                     std::size_t max_attempts = 3,
                     std::chrono::milliseconds retry_delay = std::chrono::milliseconds{200});
  ~ChatEventPublisher();

  ChatEventPublisher(const ChatEventPublisher&) = delete;
  ChatEventPublisher& operator=(const ChatEventPublisher&) = delete;

  [[nodiscard]] EnqueueStatus enqueue(std::vector<ChatMessageCreated> events);
  [[nodiscard]] PublisherSnapshot snapshot() const;
  void stop();

 private:
  struct State;

  void run();

  std::unique_ptr<State> state_;
};

}  // namespace qaiservice::persistence
