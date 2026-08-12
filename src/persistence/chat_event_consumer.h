#pragma once

#include "db/mysql_connection.h"
#include "persistence/rabbitmq_config.h"

#include <atomic>
#include <cstddef>
#include <memory>

namespace qaiservice::persistence {

struct ConsumerSnapshot {
  std::size_t persisted{0};
  std::size_t duplicates{0};
  std::size_t rejected{0};
  std::size_t connection_failures{0};
};

class ChatEventConsumer {
 public:
  ChatEventConsumer(RabbitMqConfig rabbit_config, db::DatabaseConfig database_config);
  ~ChatEventConsumer();

  ChatEventConsumer(const ChatEventConsumer&) = delete;
  ChatEventConsumer& operator=(const ChatEventConsumer&) = delete;

  [[nodiscard]] ConsumerSnapshot snapshot() const;
  void stop();

 private:
  struct State;

  void run();

  std::unique_ptr<State> state_;
};

}  // namespace qaiservice::persistence
