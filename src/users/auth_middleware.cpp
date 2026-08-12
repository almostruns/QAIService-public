#include "users/auth_middleware.h"

#include "users/session_store.h"

#include <charconv>
#include <string>
#include <string_view>
#include <utility>

namespace qaiservice::users {
namespace {

constexpr char kAuthenticatedUserHeader[] = "x-qai-authenticated-user-id";
constexpr char kSessionCookieName[] = "qai_session";

std::string_view trim(std::string_view value)
{
  while (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  while (!value.empty() && value.back() == ' ') {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<std::string> sessionToken(const http::Request& request)
{
  const auto cookie_header = request.headers.find("cookie");
  if (cookie_header == request.headers.end()) {
    return std::nullopt;
  }

  std::string_view cookies(cookie_header->second);
  while (!cookies.empty()) {
    const std::size_t separator = cookies.find(';');
    const std::string_view item = trim(cookies.substr(0, separator));
    const std::size_t equals = item.find('=');

    if (equals != std::string_view::npos && item.substr(0, equals) == kSessionCookieName) {
      return std::string(item.substr(equals + 1));
    }

    if (separator == std::string_view::npos) {
      break;
    }
    cookies.remove_prefix(separator + 1);
  }
  return std::nullopt;
}

}  // namespace

http::Middleware makeAuthenticationMiddleware(SessionStore& sessions)
{
  return [&sessions](http::Request request, http::ResponseWriter writer, http::Handler next) {
    request.headers.erase(kAuthenticatedUserHeader);
    const std::optional<std::string> token = sessionTokenFromRequest(request);

    if (token.has_value()) {
      const std::optional<std::uint64_t> user_id = sessions.findUserId(token.value());
      if (user_id.has_value()) {
        request.headers[kAuthenticatedUserHeader] = std::to_string(user_id.value());
      }
    }

    next(std::move(request), writer);
  };
}

std::optional<std::string> sessionTokenFromRequest(const http::Request& request)
{
  return sessionToken(request);
}

std::optional<std::uint64_t> authenticatedUserId(const http::Request& request)
{
  const auto header = request.headers.find(kAuthenticatedUserHeader);
  if (header == request.headers.end()) {
    return std::nullopt;
  }

  std::uint64_t user_id = 0;
  const std::string& value = header->second;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), user_id);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return user_id;
}

}  // namespace qaiservice::users
