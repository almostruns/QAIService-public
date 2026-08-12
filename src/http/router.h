#pragma once

#include "http/http_types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qaiservice::http {

// 使用精确的 (method, path) 组合匹配 Handler。
class Router {
 public:
  // 路由配置
  void add(std::string method, std::string path, Handler handler);

  // 请求分发
  void route(Request request, ResponseWriter writer) const;

 private:
  // 动态路由
  struct RouteSegment {
    bool parameter;
    std::string value;
  };

  struct DynamicRoute {
    std::string method;
    std::vector<RouteSegment> segments;
    Handler handler;
  };

  [[nodiscard]] static bool dynamicRoutesOverlap(const DynamicRoute& left, const DynamicRoute& right);
  [[nodiscard]] static std::optional<std::unordered_map<std::string, std::string>> matchDynamicRoute(
      const DynamicRoute& route, std::string_view path);

  // 路由存储
  using RouteKey = std::pair<std::string, std::string>;
  std::map<RouteKey, Handler> routes_;
  std::vector<DynamicRoute> dynamic_routes_;
};

}  // namespace qaiservice::http
