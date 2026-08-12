#pragma once

#include "persistence/chat_event_publisher.h"
#include "persistence/rabbitmq_config.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace qaiservice::persistence {

class RabbitMqError : public std::runtime_error {
 public:
  explicit RabbitMqError(const std::string& message);
};

struct RabbitDelivery {
  std::uint64_t delivery_tag;
  std::string body;
};

enum class RabbitConnectionMode {
  kPublisher,
  kConsumer,
};

class RabbitMqConnection {
 public:
  RabbitMqConnection(RabbitMqConfig config, RabbitConnectionMode mode);
  ~RabbitMqConnection();

  RabbitMqConnection(const RabbitMqConnection&) = delete;
  RabbitMqConnection& operator=(const RabbitMqConnection&) = delete;

  [[nodiscard]] TransportPublishResult publishConfirmed(const ChatMessageCreated& event);
  [[nodiscard]] std::optional<RabbitDelivery> get();
  void acknowledge(std::uint64_t delivery_tag);
  void reject(std::uint64_t delivery_tag, bool requeue);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] std::unique_ptr<ChatEventTransport> makeRabbitMqChatEventTransport(RabbitMqConfig config);

}  // namespace qaiservice::persistence
