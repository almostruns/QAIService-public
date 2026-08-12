#pragma once

#include "http/http_codec.h"
#include "http/http_types.h"

#include <string>

namespace qaiservice::http {

// HTTP 响应序列化
[[nodiscard]] std::string serializeHttpResponse(const Response& response, bool close_connection);

// 解析错误映射
[[nodiscard]] Response responseForParseError(ParseStatus status);

}  // namespace qaiservice::http
