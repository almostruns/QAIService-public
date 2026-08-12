#pragma once

#include "db/mysql_connection.h"

namespace qaiservice::db {

[[nodiscard]] DatabaseConfig databaseConfigFromEnvironment();

}  // namespace qaiservice::db
