#pragma once

#include "http/http_types.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace muduo::net {
class Buffer;
}  // namespace muduo::net

namespace qaiservice::http {

// 解析结果
enum class ParseStatus {
  kNeedMoreData,
  kComplete,
  kBadRequest,
  kPayloadTooLarge,
  kNotImplemented,
};

struct ParseResult {
  ParseStatus status;
  std::optional<Request> request;
};

// 增量 HTTP 解析器，每条 TCP 连接独享一个实例及其解析状态。
class HttpCodec {
 public:
  // 解析入口
  [[nodiscard]] ParseResult parse(muduo::net::Buffer& buffer);

 private:
  // 解析状态机：TCP 分片陆续到达时依次推进。
  enum class State {
    kRequestLine,
    kHeaders,
    kBody,
    kError,
  };

  // 分段解析
  [[nodiscard]] bool parseRequestLine(std::string_view line);
  [[nodiscard]] bool parseHeader(std::string_view line);
  void reset();

  // 当前请求状态
  State state_{State::kRequestLine};
  Request request_;
  std::size_t content_length_{0};
  std::size_t header_bytes_{0};
};

}  // namespace qaiservice::http
