#pragma once

#include <mysql.h>

#include <string>

namespace qaiservice::db {

// Prepared Statement 的 RAII 包装：构造时 prepare，析构时释放，禁止拷贝。
class PreparedStatement {
 public:
  PreparedStatement(MYSQL* connection, const char* sql);
  ~PreparedStatement();

  PreparedStatement(const PreparedStatement&) = delete;
  PreparedStatement& operator=(const PreparedStatement&) = delete;

  [[nodiscard]] MYSQL_STMT* get() const;

 private:
  MYSQL_STMT* statement_{nullptr};
};

void bindString(MYSQL_BIND& binding, const std::string& value, unsigned long& length);
void executeQuery(MYSQL* connection, const char* query, const char* error);

}  // namespace qaiservice::db
