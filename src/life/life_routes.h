#pragma once

namespace qaiservice::db {
class DatabaseWorker;
}

namespace qaiservice::http {
class Router;
}

namespace qaiservice::life {

void registerLifeRoutes(http::Router& router, db::DatabaseWorker& database_worker);

}  // namespace qaiservice::life
