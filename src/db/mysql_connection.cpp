#include "db/mysql_connection.h"

#include <mysql.h>

namespace qaiservice::db {

DatabaseError::DatabaseError(const std::string& message) : std::runtime_error(message)
{
}

MySqlConnection::MySqlConnection(const DatabaseConfig& config)
{
  connection_ = mysql_init(nullptr);
  if (connection_ == nullptr) {
    throw DatabaseError("cannot initialize MySQL connection");
  }

  const char* character_set = "utf8mb4";
  unsigned int timeout_seconds = 5;
  const bool options_failed = mysql_options(connection_, MYSQL_SET_CHARSET_NAME, character_set) != 0 ||
                              mysql_options(connection_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds) != 0 ||
                              mysql_options(connection_, MYSQL_OPT_READ_TIMEOUT, &timeout_seconds) != 0 ||
                              mysql_options(connection_, MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds) != 0;
  if (options_failed) {
    mysql_close(connection_);
    connection_ = nullptr;
    throw DatabaseError("cannot configure MySQL connection");
  }

  MYSQL* connected = mysql_real_connect(connection_, config.host.c_str(), config.user.c_str(), config.password.c_str(),
                                        config.database.c_str(), config.port, nullptr, 0);
  if (connected == nullptr) {
    mysql_close(connection_);
    connection_ = nullptr;
    throw DatabaseError("cannot connect to MySQL");
  }
}

MySqlConnection::~MySqlConnection()
{
  if (connection_ != nullptr) {
    mysql_close(connection_);
  }
}

st_mysql* MySqlConnection::nativeHandle() const
{
  return connection_;
}

}  // namespace qaiservice::db
