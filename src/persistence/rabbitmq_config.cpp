#include "persistence/rabbitmq_config.h"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace qaiservice::persistence {
namespace {

std::string readOrDefault(const RabbitEnvironmentReader& reader, const std::string& name,
                          const std::string& default_value)
{
  const std::optional<std::string> value = reader(name);
  if (!value.has_value() || value->empty()) {
    return default_value;
  }
  return value.value();
}

int parsePort(const std::string& value)
{
  int port = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port <= 0 || port > 65535) {
    throw std::invalid_argument("QAI_RABBITMQ_PORT must be between 1 and 65535");
  }
  return port;
}

}  // namespace

RabbitMqConfig loadRabbitMqConfig(const RabbitEnvironmentReader& read_environment)
{
  RabbitMqConfig config;
  config.host = readOrDefault(read_environment, "QAI_RABBITMQ_HOST", "rabbitmq");
  config.port = parsePort(readOrDefault(read_environment, "QAI_RABBITMQ_PORT", "5672"));
  config.user = readOrDefault(read_environment, "QAI_RABBITMQ_USER", "qaiservice");
  config.password = readOrDefault(read_environment, "QAI_RABBITMQ_PASSWORD", "");
  config.vhost = readOrDefault(read_environment, "QAI_RABBITMQ_VHOST", "/");
  config.queue = readOrDefault(read_environment, "QAI_RABBITMQ_QUEUE", "qaiservice.chat-messages");

  if (config.password.empty()) {
    throw std::invalid_argument("QAI_RABBITMQ_PASSWORD is required");
  }
  return config;
}

RabbitMqConfig rabbitMqConfigFromEnvironment()
{
  RabbitEnvironmentReader reader = [](const std::string& name) -> std::optional<std::string> {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
  return loadRabbitMqConfig(reader);
}

}  // namespace qaiservice::persistence
