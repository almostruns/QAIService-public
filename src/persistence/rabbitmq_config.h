#pragma once

#include <functional>
#include <optional>
#include <string>

namespace qaiservice::persistence {

struct RabbitMqConfig {
  std::string host;
  int port;
  std::string user;
  std::string password;
  std::string vhost;
  std::string queue;
};

using RabbitEnvironmentReader = std::function<std::optional<std::string>(const std::string&)>;

[[nodiscard]] RabbitMqConfig loadRabbitMqConfig(const RabbitEnvironmentReader& read_environment);
[[nodiscard]] RabbitMqConfig rabbitMqConfigFromEnvironment();

}  // namespace qaiservice::persistence
