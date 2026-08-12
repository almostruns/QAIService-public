#pragma once

#include "http/middleware.h"

#include <cstdint>
#include <optional>
#include <string>

namespace qaiservice::users {

class SessionStore;

[[nodiscard]] http::Middleware makeAuthenticationMiddleware(SessionStore& sessions);
[[nodiscard]] std::optional<std::uint64_t> authenticatedUserId(const http::Request& request);
[[nodiscard]] std::optional<std::string> sessionTokenFromRequest(const http::Request& request);

}  // namespace qaiservice::users
