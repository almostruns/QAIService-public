#include "persistence/rabbitmq_connection.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#include <sys/time.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace qaiservice::persistence {
namespace {

constexpr amqp_channel_t kChannel = 1;

amqp_bytes_t bytes(const std::string& value)
{
  return amqp_bytes_t{value.size(), const_cast<char*>(value.data())};
}

void requireNormalReply(amqp_connection_state_t connection, const char* message)
{
  const amqp_rpc_reply_t reply = amqp_get_rpc_reply(connection);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    throw RabbitMqError(message);
  }
}

class RabbitMqChatEventTransport final : public ChatEventTransport {
 public:
  explicit RabbitMqChatEventTransport(RabbitMqConfig config) : config_(std::move(config))
  {
  }

  TransportPublishResult publish(const ChatMessageCreated& event) override
  {
    try {
      if (connection_ == nullptr) {
        connection_ = std::make_unique<RabbitMqConnection>(config_, RabbitConnectionMode::kPublisher);
      }
      const TransportPublishResult result = connection_->publishConfirmed(event);
      if (result != TransportPublishResult::kConfirmed) {
        connection_.reset();
      }
      return result;
    } catch (const RabbitMqError&) {
      connection_.reset();
      return TransportPublishResult::kRetryableFailure;
    }
  }

 private:
  RabbitMqConfig config_;
  std::unique_ptr<RabbitMqConnection> connection_;
};

}  // namespace

struct RabbitMqConnection::State {
  RabbitMqConfig config;
  RabbitConnectionMode mode;
  amqp_connection_state_t connection{nullptr};
  amqp_socket_t* socket{nullptr};
};

RabbitMqError::RabbitMqError(const std::string& message) : std::runtime_error(message)
{
}

RabbitMqConnection::RabbitMqConnection(RabbitMqConfig config, RabbitConnectionMode mode)
    : state_(std::make_unique<State>())
{
  state_->config = std::move(config);
  state_->mode = mode;
  state_->connection = amqp_new_connection();
  if (state_->connection == nullptr) {
    throw RabbitMqError("cannot create RabbitMQ connection");
  }
  try {
    state_->socket = amqp_tcp_socket_new(state_->connection);
    timeval connect_timeout{3, 0};
    const int connected = state_->socket == nullptr
                              ? AMQP_STATUS_SOCKET_ERROR
                              : amqp_socket_open_noblock(state_->socket, state_->config.host.c_str(),
                                                         state_->config.port, &connect_timeout);
    if (connected != AMQP_STATUS_OK) {
      throw RabbitMqError("cannot connect to RabbitMQ");
    }

    const amqp_rpc_reply_t login = amqp_login(state_->connection, state_->config.vhost.c_str(), 0, 131072, 5,
                                               AMQP_SASL_METHOD_PLAIN, state_->config.user.c_str(),
                                               state_->config.password.c_str());
    if (login.reply_type != AMQP_RESPONSE_NORMAL) {
      throw RabbitMqError("cannot authenticate to RabbitMQ");
    }

    amqp_channel_open(state_->connection, kChannel);
    requireNormalReply(state_->connection, "cannot open RabbitMQ channel");
    amqp_queue_declare(state_->connection, kChannel, bytes(state_->config.queue), 0, 1, 0, 0, amqp_empty_table);
    requireNormalReply(state_->connection, "cannot declare RabbitMQ queue");

    if (mode == RabbitConnectionMode::kPublisher) {
      amqp_confirm_select(state_->connection, kChannel);
      requireNormalReply(state_->connection, "cannot enable RabbitMQ publisher confirms");
    }
  } catch (...) {
    amqp_destroy_connection(state_->connection);
    state_->connection = nullptr;
    throw;
  }
}

RabbitMqConnection::~RabbitMqConnection()
{
  if (state_->connection != nullptr) {
    amqp_channel_close(state_->connection, kChannel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(state_->connection, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(state_->connection);
  }
}

TransportPublishResult RabbitMqConnection::publishConfirmed(const ChatMessageCreated& event)
{
  if (state_->mode != RabbitConnectionMode::kPublisher) {
    throw RabbitMqError("RabbitMQ connection is not a publisher");
  }

  const std::string body = chatMessageEventToJson(event);
  amqp_basic_properties_t properties{};
  properties._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
  properties.content_type = amqp_cstring_bytes("application/json");
  properties.delivery_mode = 2;
  const int published = amqp_basic_publish(state_->connection, kChannel, amqp_empty_bytes,
                                            bytes(state_->config.queue), 0, 0, &properties, bytes(body));
  if (published != AMQP_STATUS_OK) {
    return TransportPublishResult::kRetryableFailure;
  }

  amqp_frame_t frame{};
  timeval timeout{3, 0};
  const int waited = amqp_simple_wait_frame_noblock(state_->connection, &frame, &timeout);
  if (waited != AMQP_STATUS_OK || frame.frame_type != AMQP_FRAME_METHOD) {
    return TransportPublishResult::kRetryableFailure;
  }
  if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD) {
    return TransportPublishResult::kConfirmed;
  }
  if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
    return TransportPublishResult::kRetryableFailure;
  }
  return TransportPublishResult::kRetryableFailure;
}

std::optional<RabbitDelivery> RabbitMqConnection::get()
{
  if (state_->mode != RabbitConnectionMode::kConsumer) {
    throw RabbitMqError("RabbitMQ connection is not a consumer");
  }

  const amqp_rpc_reply_t reply = amqp_basic_get(state_->connection, kChannel, bytes(state_->config.queue), 0);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    throw RabbitMqError("cannot get RabbitMQ message");
  }
  if (reply.reply.id == AMQP_BASIC_GET_EMPTY_METHOD) {
    return std::nullopt;
  }
  if (reply.reply.id != AMQP_BASIC_GET_OK_METHOD) {
    throw RabbitMqError("unexpected RabbitMQ get response");
  }

  const auto* get_ok = static_cast<amqp_basic_get_ok_t*>(reply.reply.decoded);
  const std::uint64_t delivery_tag = get_ok->delivery_tag;
  amqp_message_t message{};
  const amqp_rpc_reply_t read = amqp_read_message(state_->connection, kChannel, &message, 0);
  if (read.reply_type != AMQP_RESPONSE_NORMAL) {
    throw RabbitMqError("cannot read RabbitMQ message");
  }
  std::string body(static_cast<char*>(message.body.bytes), message.body.len);
  amqp_destroy_message(&message);
  return RabbitDelivery{delivery_tag, std::move(body)};
}

void RabbitMqConnection::acknowledge(std::uint64_t delivery_tag)
{
  if (amqp_basic_ack(state_->connection, kChannel, delivery_tag, 0) != AMQP_STATUS_OK) {
    throw RabbitMqError("cannot acknowledge RabbitMQ message");
  }
}

void RabbitMqConnection::reject(std::uint64_t delivery_tag, bool requeue)
{
  if (amqp_basic_nack(state_->connection, kChannel, delivery_tag, 0, requeue ? 1 : 0) != AMQP_STATUS_OK) {
    throw RabbitMqError("cannot reject RabbitMQ message");
  }
}

std::unique_ptr<ChatEventTransport> makeRabbitMqChatEventTransport(RabbitMqConfig config)
{
  return std::make_unique<RabbitMqChatEventTransport>(std::move(config));
}

}  // namespace qaiservice::persistence
