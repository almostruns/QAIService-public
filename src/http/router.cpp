#include "http/router.h"

#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qaiservice::http {
namespace {

std::vector<std::string_view> splitPath(std::string_view path)
{
  if (path.empty() || path.front() != '/') {
    throw std::logic_error("route path must start with /");
  }

  std::vector<std::string_view> segments;
  std::size_t segment_begin = 1;
  while (segment_begin <= path.size()) {
    const std::size_t separator = path.find('/', segment_begin);
    if (separator == std::string_view::npos) {
      segments.push_back(path.substr(segment_begin));
      break;
    }

    segments.push_back(path.substr(segment_begin, separator - segment_begin));
    segment_begin = separator + 1;
  }
  return segments;
}

bool isParameter(std::string_view segment)
{
  return segment.size() >= 3 && segment.front() == '{' && segment.back() == '}';
}

}  // namespace

bool Router::dynamicRoutesOverlap(const DynamicRoute& left, const DynamicRoute& right)
{
  if (left.method != right.method || left.segments.size() != right.segments.size()) {
    return false;
  }

  for (std::size_t index = 0; index < left.segments.size(); ++index) {
    const RouteSegment& left_segment = left.segments[index];
    const RouteSegment& right_segment = right.segments[index];
    if (!left_segment.parameter && !right_segment.parameter && left_segment.value != right_segment.value) {
      return false;
    }
  }
  return true;
}

std::optional<std::unordered_map<std::string, std::string>> Router::matchDynamicRoute(
    const DynamicRoute& route, std::string_view path)
{
  const std::vector<std::string_view> path_segments = splitPath(path);
  if (path_segments.size() != route.segments.size()) {
    return std::nullopt;
  }

  std::unordered_map<std::string, std::string> parameters;
  for (std::size_t index = 0; index < route.segments.size(); ++index) {
    const RouteSegment& route_segment = route.segments[index];
    const std::string_view path_segment = path_segments[index];
    if (route_segment.parameter) {
      if (path_segment.empty()) {
        return std::nullopt;
      }
      parameters.emplace(route_segment.value, path_segment);
      continue;
    }

    if (route_segment.value != path_segment) {
      return std::nullopt;
    }
  }
  return parameters;
}

// 路由配置
void Router::add(std::string method, std::string path, Handler handler)
{
  const std::vector<std::string_view> segment_views = splitPath(path);
  bool has_parameter = false;
  std::vector<RouteSegment> segments;
  std::unordered_set<std::string> parameter_names;
  segments.reserve(segment_views.size());

  for (const std::string_view segment : segment_views) {
    if (isParameter(segment)) {
      has_parameter = true;
      const std::string parameter_name(segment.substr(1, segment.size() - 2));
      if (!parameter_names.insert(parameter_name).second) {
        throw std::logic_error("duplicate route parameter: " + parameter_name);
      }
      segments.push_back({true, parameter_name});
      continue;
    }

    if (segment.find('{') != std::string_view::npos || segment.find('}') != std::string_view::npos) {
      throw std::logic_error("invalid route parameter segment");
    }
    segments.push_back({false, std::string(segment)});
  }

  if (has_parameter) {
    DynamicRoute dynamic_route{std::move(method), std::move(segments), std::move(handler)};
    for (const DynamicRoute& existing : dynamic_routes_) {
      if (dynamicRoutesOverlap(existing, dynamic_route)) {
        throw std::logic_error("ambiguous dynamic route");
      }
    }
    dynamic_routes_.push_back(std::move(dynamic_route));
    return;
  }

  const RouteKey key{std::move(method), std::move(path)};
  const bool inserted = routes_.emplace(key, std::move(handler)).second;

  if (!inserted) {
    // 重复路由属于启动配置错误，不是运行时 HTTP 错误。
    throw std::logic_error("duplicate route: " + key.first + " " + key.second);
  }
}

// 请求分发
void Router::route(Request request, ResponseWriter writer) const
{
  const RouteKey key{request.method, request.path};
  const auto route = routes_.find(key);

  if (route == routes_.end()) {
    for (const DynamicRoute& dynamic_route : dynamic_routes_) {
      if (dynamic_route.method != request.method) {
        continue;
      }

      auto parameters = matchDynamicRoute(dynamic_route, request.path);
      if (!parameters.has_value()) {
        continue;
      }

      request.path_parameters = std::move(parameters.value());
      dynamic_route.handler(std::move(request), writer);
      return;
    }

    writer.send(Response{404, "application/json", R"({"error":"not_found"})"});
    return;
  }

  route->second(std::move(request), writer);
}

}  // namespace qaiservice::http
