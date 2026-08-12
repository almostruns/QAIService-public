#include "db/mysql_statement.h"

#include "db/mysql_connection.h"

#include <cstring>

namespace qaiservice::db {

PreparedStatement::PreparedStatement(MYSQL* connection, const char* sql)
{
  statement_ = mysql_stmt_init(connection);
  if (statement_ == nullptr) {
    throw DatabaseError("cannot initialize prepared statement");
  }
  const unsigned long sql_length = static_cast<unsigned long>(std::strlen(sql));
  if (mysql_stmt_prepare(statement_, sql, sql_length) != 0) {
    mysql_stmt_close(statement_);
    statement_ = nullptr;
    throw DatabaseError("cannot prepare database statement");
  }
}

PreparedStatement::~PreparedStatement()
{
  if (statement_ != nullptr) {
    mysql_stmt_close(statement_);
  }
}

MYSQL_STMT* PreparedStatement::get() const
{
  return statement_;
}

void bindString(MYSQL_BIND& binding, const std::string& value, unsigned long& length)
{
  length = static_cast<unsigned long>(value.size());
  binding.buffer_type = MYSQL_TYPE_STRING;
  binding.buffer = const_cast<char*>(value.data());
  binding.buffer_length = length;
  binding.length = &length;
}

void executeQuery(MYSQL* connection, const char* query, const char* error)
{
  if (mysql_query(connection, query) != 0) {
    throw DatabaseError(error);
  }
}

}  // namespace qaiservice::db
