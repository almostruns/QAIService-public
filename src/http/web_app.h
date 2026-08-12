#pragma once

#include <filesystem>
#include <functional>

namespace qaiservice::http {

class Router;
struct Request;

using PageAccessCheck = std::function<bool(const Request&)>;

struct WebAppConfig {
  bool registration_enabled{true};
  PageAccessCheck authenticated;
};

void registerWebAppRoutes(Router& router, const std::filesystem::path& web_root, WebAppConfig config = {});

}  // namespace qaiservice::http
