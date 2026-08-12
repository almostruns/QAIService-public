#include "users/auth_routes.h"

#include "db/database_worker.h"
#include "http/json_response.h"
#include "http/router.h"
#include "users/auth_middleware.h"
#include "users/password_hasher.h"
#include "users/session_repository.h"
#include "users/session_store.h"
#include "users/user_repository.h"
#include "util/time.h"

#include "logging/log.h"
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace qaiservice::users {
namespace {

struct Credentials {
  std::string username;
  std::string password;
};

bool validUsername(const std::string& username)
{
  if (username.size() < 3 || username.size() > 64) {
    return false;
  }

  for (const unsigned char value : username) {
    if (!std::isalnum(value) && value != '_' && value != '-') {
      return false;
    }
  }
  return true;
}

std::optional<Credentials> parseCredentials(const http::Request& request)
{
  try {
    const nlohmann::json body = nlohmann::json::parse(request.body);
    if (!body.is_object() || !body.contains("username") || !body.contains("password")) {
      return std::nullopt;
    }
    if (!body["username"].is_string() || !body["password"].is_string()) {
      return std::nullopt;
    }

    Credentials credentials{body["username"].get<std::string>(), body["password"].get<std::string>()};
    if (!validUsername(credentials.username) || credentials.password.size() < 8 || credentials.password.size() > 128) {
      return std::nullopt;
    }
    return credentials;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

std::string sessionCookie(const std::string& token, bool secure_cookie)
{
  std::string cookie =
      "qai_session=" + token + "; Max-Age=" + std::to_string(kSessionLifetime.count()) + "; HttpOnly; SameSite=Lax; Path=/";
  if (secure_cookie) {
    cookie += "; Secure";
  }
  return cookie;
}

std::string expiredSessionCookie()
{
  return "qai_session=; Max-Age=0; HttpOnly; SameSite=Lax; Path=/";
}

void registerRegistrationRoute(http::Router& router, db::DatabaseWorker& database_worker, AuthRouteConfig config)
{
  router.add("POST", "/api/register", [&database_worker, config](http::Request request, http::ResponseWriter writer) {
    if (!config.registration_enabled) {
      writer.send(http::jsonResponse(403, {{"error", "registration_disabled"}}));
      return;
    }

    const std::optional<Credentials> credentials = parseCredentials(request);
    if (!credentials.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_registration"}}));
      return;
    }

    db::DatabaseWorker::Task task = [credentials = credentials.value(), writer](db::MySqlConnection& connection) {
      try {
        PasswordHasher hasher;
        const std::string password_hash = hasher.hash(credentials.password);
        UserRepository repository(connection);
        const CreateUserResult result = repository.create(credentials.username, password_hash);

        if (result.status == CreateUserStatus::kUsernameExists) {
          writer.send(http::jsonResponse(409, {{"error", "username_exists"}}));
          return;
        }
        writer.send(http::jsonResponse(201, {{"user_id", result.user->id}, {"username", result.user->username}}));
      } catch (const db::DatabaseError&) {
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, "users") << "user_register username=" << credentials.username << " error=" << error.what();
        writer.send(http::jsonResponse(500, {{"error", "internal_server_error"}}));
      }
    };

    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });
}

void registerLoginRoute(http::Router& router, db::DatabaseWorker& database_worker, SessionStore& sessions,
                        AuthenticationSuccessHook authentication_success_hook, AuthRouteConfig config)
{
  router.add("POST", "/api/login",
             [&database_worker, &sessions, authentication_success_hook = std::move(authentication_success_hook),
              config](http::Request request, http::ResponseWriter writer) {
    const std::optional<Credentials> credentials = parseCredentials(request);
    if (!credentials.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_login"}}));
      return;
    }

    db::DatabaseWorker::Task task =
        [credentials = credentials.value(), &sessions, authentication_success_hook, config, writer](
            db::MySqlConnection& connection) {
          try {
            UserRepository repository(connection);
            const std::optional<User> user = repository.findByUsername(credentials.username);
            PasswordHasher hasher;

            if (!user.has_value() || !hasher.verify(user->password_hash, credentials.password)) {
              writer.send(http::jsonResponse(401, {{"error", "invalid_credentials"}}));
              return;
            }

            if (authentication_success_hook) {
              authentication_success_hook(user->id, connection);
            }

            const Session session = sessions.create(user->id);
            try {
              SessionRepository session_repository(connection);
              const std::int64_t created_at_ms = util::currentTimeMilliseconds();
              const std::int64_t expires_at_ms =
                  std::chrono::duration_cast<std::chrono::milliseconds>(session.expires_at.time_since_epoch()).count();
              session_repository.create(hashSessionToken(session.token), user->id, created_at_ms, expires_at_ms);
            } catch (...) {
              // 落库失败的 session 不允许只留在内存里，否则重启后行为不一致。
              sessions.erase(session.token);
              throw;
            }

            http::Response response = http::jsonResponse(200, {{"user_id", user->id}, {"username", user->username}});
            response.headers["Set-Cookie"] = sessionCookie(session.token, config.secure_cookie);
            writer.send(std::move(response));
          } catch (const db::DatabaseError&) {
            writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
          } catch (const std::exception& error) {
            QAI_LOG(err, "users") << "user_login username=" << credentials.username << " error=" << error.what();
            writer.send(http::jsonResponse(500, {{"error", "internal_server_error"}}));
          }
        };

    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
             });
}

void registerSessionRoutes(http::Router& router, db::DatabaseWorker& database_worker, SessionStore& sessions)
{
  router.add("GET", "/api/me", [](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    writer.send(http::jsonResponse(200, {{"user_id", user_id.value()}}));
  });

  router.add("POST", "/api/logout", [&database_worker, &sessions](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::string> token = sessionTokenFromRequest(request);
    if (!token.has_value()) {
      http::Response response{204, "application/json", ""};
      response.headers["Cache-Control"] = "no-store";
      response.headers["Set-Cookie"] = expiredSessionCookie();
      writer.send(std::move(response));
      return;
    }

    sessions.erase(token.value());
    const std::string token_hash = hashSessionToken(token.value());
    db::DatabaseWorker::Task task = [token_hash, writer](db::MySqlConnection& connection) {
      try {
        SessionRepository repository(connection);
        repository.erase(token_hash);
        http::Response response{204, "application/json", ""};
        response.headers["Cache-Control"] = "no-store";
        response.headers["Set-Cookie"] = expiredSessionCookie();
        writer.send(std::move(response));
      } catch (const std::exception& error) {
        QAI_LOG(warn, "users") << "session_logout_persist error=" << error.what();
        http::Response response = http::jsonResponse(503, {{"error", "session_logout_unavailable"}});
        response.headers["Cache-Control"] = "no-store";
        response.headers["Set-Cookie"] = expiredSessionCookie();
        writer.send(std::move(response));
      }
    };

    if (!database_worker.submit(std::move(task))) {
      http::Response response = http::jsonResponse(503, {{"error", "database_busy"}});
      response.headers["Cache-Control"] = "no-store";
      response.headers["Set-Cookie"] = expiredSessionCookie();
      writer.send(std::move(response));
    }
  });
}

}  // namespace

AuthRouteConfig loadAuthRouteConfig(const AuthEnvironmentReader& read_environment)
{
  auto read_boolean = [&read_environment](const std::string& name, bool default_value) {
    const std::optional<std::string> value = read_environment(name);
    if (!value.has_value() || value->empty()) {
      return default_value;
    }
    if (value.value() == "true") {
      return true;
    }
    if (value.value() == "false") {
      return false;
    }
    throw std::invalid_argument(name + " must be true or false");
  };

  const bool secure_cookie = read_boolean("QAI_COOKIE_SECURE", false);
  const bool registration_enabled = read_boolean("QAI_REGISTRATION_ENABLED", true);
  return {secure_cookie, registration_enabled};
}

AuthRouteConfig authRouteConfigFromEnvironment()
{
  AuthEnvironmentReader reader = [](const std::string& name) -> std::optional<std::string> {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
  return loadAuthRouteConfig(reader);
}

void registerAuthRoutes(http::Router& router, db::DatabaseWorker& database_worker, SessionStore& sessions,
                        AuthenticationSuccessHook authentication_success_hook, AuthRouteConfig config)
{
  registerRegistrationRoute(router, database_worker, config);
  registerLoginRoute(router, database_worker, sessions, std::move(authentication_success_hook), config);
  registerSessionRoutes(router, database_worker, sessions);
}

}  // namespace qaiservice::users
