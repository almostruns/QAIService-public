#include "db/database_config.h"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace qaiservice::db {
namespace {

std::string requiredEnvironment(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    throw std::runtime_error(std::string("missing required environment variable: ") + name);
  }
  return value;
}

unsigned int databasePort()
{
  const char* value = std::getenv("QAI_DB_PORT");
  if (value == nullptr || value[0] == '\0') {
    return 3306;
  }

  unsigned int port = 0;
  const std::string text(value);
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port == 0 || port > 65535) {
    throw std::runtime_error("QAI_DB_PORT must be an integer between 1 and 65535");
  }
  return port;
}

}  // namespace

DatabaseConfig databaseConfigFromEnvironment()
{
  DatabaseConfig config;
  config.host = requiredEnvironment("QAI_DB_HOST");
  config.port = databasePort();
  config.database = requiredEnvironment("QAI_DB_NAME");
  config.user = requiredEnvironment("QAI_DB_USER");
  config.password = requiredEnvironment("QAI_DB_PASSWORD");
  return config;
}

}  // namespace qaiservice::db
