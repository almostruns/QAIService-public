#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace qaiservice::http {

// HTTP 数据模型
// Request 持有全部解析数据，Muduo 复用 Buffer 后数据仍然有效。
struct Request {
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::string version;
  std::unordered_map<std::string, std::string> path_parameters{};
  std::string query{};
};

struct Response {
  int status_code;
  std::string content_type;
  std::string body;
  std::unordered_map<std::string, std::string> headers{};
};

// 响应发送
// Handler 只依赖可复制的 Writer，不持有 socket，因此同步和延迟响应使用同一接口。
class ResponseWriter {
 public:
  using SendFunction = std::function<void(Response)>;

  // Writer 生命周期与发送
  explicit ResponseWriter(SendFunction send_function);
  void send(Response response) const;

 private:
  // 共享发送状态
  struct State;
  std::shared_ptr<State> state_;
};

// 请求处理
using Handler = std::function<void(Request, ResponseWriter)>;

}  // namespace qaiservice::http
