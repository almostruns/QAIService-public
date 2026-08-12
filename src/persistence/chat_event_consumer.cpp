#include "persistence/chat_event_consumer.h"

#include "persistence/chat_message_event.h"
#include "persistence/chat_message_repository.h"
#include "persistence/rabbitmq_connection.h"

#include <mysql.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace qaiservice::persistence {
namespace {

class MySqlThreadContext {
 public:
  MySqlThreadContext()
  {
    if (mysql_thread_init() != 0) {
      throw db::DatabaseError("cannot initialize persistence MySQL thread");
    }
  }

  ~MySqlThreadContext()
  {
    mysql_thread_end();
  }
};

}  // namespace

struct ChatEventConsumer::State {
  State(RabbitMqConfig rabbit, db::DatabaseConfig database)
      : rabbit_config(std::move(rabbit)), database_config(std::move(database))
  {
  }

  RabbitMqConfig rabbit_config;
  db::DatabaseConfig database_config;
  mutable std::mutex mutex;
  ConsumerSnapshot snapshot;
  std::atomic_bool stopping{false};
  std::thread thread;
};

ChatEventConsumer::ChatEventConsumer(RabbitMqConfig rabbit_config, db::DatabaseConfig database_config)
    : state_(std::make_unique<State>(std::move(rabbit_config), std::move(database_config)))
{
  state_->thread = std::thread([this] {
    run();
  });
}

ChatEventConsumer::~ChatEventConsumer()
{
  stop();
}

ConsumerSnapshot ChatEventConsumer::snapshot() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->snapshot;
}

void ChatEventConsumer::stop()
{
  state_->stopping.store(true);
  if (state_->thread.joinable()) {
    state_->thread.join();
  }
}

void ChatEventConsumer::run()
{
  try {
    MySqlThreadContext thread_context;
    while (!state_->stopping.load()) {
      try {
        RabbitMqConnection rabbit(state_->rabbit_config, RabbitConnectionMode::kConsumer);
        db::MySqlConnection database(state_->database_config);
        ChatMessageRepository repository(database);

        while (!state_->stopping.load()) {
          const std::optional<RabbitDelivery> delivery = rabbit.get();
          if (!delivery.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
          }

          const std::optional<ChatMessageCreated> event = chatMessageEventFromJson(delivery->body);
          if (!event.has_value()) {
            rabbit.reject(delivery->delivery_tag, false);
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->snapshot.rejected;
            continue;
          }

          const bool inserted = repository.insertIdempotent(event.value());
          rabbit.acknowledge(delivery->delivery_tag);
          std::lock_guard<std::mutex> lock(state_->mutex);
          if (inserted) {
            ++state_->snapshot.persisted;
          } else {
            ++state_->snapshot.duplicates;
          }
        }
      } catch (const std::exception&) {
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          ++state_->snapshot.connection_failures;
        }
        if (!state_->stopping.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds{250});
        }
      }
    }
  } catch (const std::exception&) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->snapshot.connection_failures;
  }
}

}  // namespace qaiservice::persistence
