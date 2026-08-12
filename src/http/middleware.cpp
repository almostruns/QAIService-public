#include "http/middleware.h"

#include "logging/log.h"

#include <atomic>
#include <chrono>
#include <utility>

namespace qaiservice::http {

// 中间件链
void MiddlewareChain::add(Middleware middleware)
{
  middleware_.push_back(std::move(middleware));
}

Handler MiddlewareChain::compose(Handler terminal_handler) const
{
  Handler handler = std::move(terminal_handler);

  for (auto middleware = middleware_.rbegin(); middleware != middleware_.rend(); ++middleware) {
    Handler next = std::move(handler);
    handler = [current = *middleware, next = std::move(next)](Request request, ResponseWriter writer) {
      current(std::move(request), writer, next);
    };
  }

  return handler;
}

// 内置中间件
Middleware makeAccessLogMiddleware(AccessLogSink sink)
{
  return [sink = std::move(sink)](Request request, ResponseWriter writer, Handler next) {
    static std::atomic<std::uint64_t> next_request_id{1};
    const std::uint64_t request_id = next_request_id.fetch_add(1, std::memory_order_relaxed);
    const std::string method = request.method;
    const std::string path = request.path;
    const auto started_at = std::chrono::steady_clock::now();
    ResponseWriter logged_writer([writer, sink, request_id, method, path, started_at](Response response) {
      const auto completed_at = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(completed_at - started_at);
      AccessLogEntry entry{request_id, method, path, response.status_code, elapsed.count()};
      sink(std::move(entry));
      writer.send(std::move(response));
    });
    next(std::move(request), logged_writer);
  };
}

Middleware makeErrorMiddleware()
{
  return [](Request request, ResponseWriter writer, Handler next) {
    try {
      next(std::move(request), writer);
    } catch (const std::exception& error) {
      QAI_LOG(err, "http") << "http_handler_exception path=" << request.path << " error=" << error.what();
      writer.send(Response{500, "application/json", R"({"error":"internal_server_error"})"});
    }
  };
}

}  // namespace qaiservice::http
