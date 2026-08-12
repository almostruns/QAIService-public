#pragma once

#include <stdexcept>
#include <string>

struct st_mysql;

namespace qaiservice::db {

struct DatabaseConfig {
  std::string host;
  unsigned int port{3306};
  std::string database;
  std::string user;
  std::string password;
};

class DatabaseError : public std::runtime_error {
 public:
  explicit DatabaseError(const std::string& message);
};

class MySqlConnection {
 public:
  explicit MySqlConnection(const DatabaseConfig& config);
  ~MySqlConnection();

  MySqlConnection(const MySqlConnection&) = delete;
  MySqlConnection& operator=(const MySqlConnection&) = delete;

  [[nodiscard]] st_mysql* nativeHandle() const;

 private:
  st_mysql* connection_{nullptr};
};

}  // namespace qaiservice::db
