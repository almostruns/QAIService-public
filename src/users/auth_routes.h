#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace qaiservice::db {
class DatabaseWorker;
class MySqlConnection;
}

namespace qaiservice::http {
class Router;
}

namespace qaiservice::users {

class SessionStore;

using AuthenticationSuccessHook = std::function<void(std::uint64_t, db::MySqlConnection&)>;
using AuthEnvironmentReader = std::function<std::optional<std::string>(const std::string&)>;

struct AuthRouteConfig {
  bool secure_cookie{false};
  bool registration_enabled{true};
};

[[nodiscard]] AuthRouteConfig loadAuthRouteConfig(const AuthEnvironmentReader& read_environment);
[[nodiscard]] AuthRouteConfig authRouteConfigFromEnvironment();

void registerAuthRoutes(http::Router& router, db::DatabaseWorker& database_worker, SessionStore& sessions,
                        AuthenticationSuccessHook authentication_success_hook = {}, AuthRouteConfig config = {});

}  // namespace qaiservice::users
