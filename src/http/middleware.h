#pragma once

#include "http/http_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qaiservice::http {

// 中间件抽象
using Middleware = std::function<void(Request, ResponseWriter, Handler)>;

class MiddlewareChain {
 public:
  // 中间件配置
  void add(Middleware middleware);

  // Handler 组合
  [[nodiscard]] Handler compose(Handler terminal_handler) const;

 private:
  // 注册顺序
  std::vector<Middleware> middleware_;
};

// 内置中间件
struct AccessLogEntry {
  std::uint64_t request_id;
  std::string method;
  std::string path;
  int status_code;
  std::int64_t elapsed_milliseconds;
};

using AccessLogSink = std::function<void(AccessLogEntry)>;

[[nodiscard]] Middleware makeAccessLogMiddleware(AccessLogSink sink);
[[nodiscard]] Middleware makeErrorMiddleware();

}  // namespace qaiservice::http
