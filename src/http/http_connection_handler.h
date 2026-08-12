#pragma once

#include "http/http_codec.h"

#include <optional>

namespace muduo::net {
class Buffer;
}  // namespace muduo::net

namespace qaiservice::http {

// 连接输入处理结果
struct ConnectionProcessResult {
  // 连接控制
  // 保持连接打开，等待下一个 TCP 分片。
  bool needs_more_data;
  // 序列化响应进入发送队列后关闭连接。
  bool close_connection;

  // 解析产物
  std::optional<Request> request;
  std::optional<Response> error_response;
};

// 连接输入处理
[[nodiscard]] ConnectionProcessResult processHttpInput(HttpCodec& codec, muduo::net::Buffer& buffer);

}  // namespace qaiservice::http
