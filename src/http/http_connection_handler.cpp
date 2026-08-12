#include "http/http_connection_handler.h"

#include "http/http_response_writer.h"

#include <muduo/net/Buffer.h>

#include <cctype>
#include <string>
#include <utility>

namespace qaiservice::http {
namespace {

// Header 处理
std::string lowercase(std::string value)
{
  for (char& character : value) {
    const unsigned char unsigned_character = static_cast<unsigned char>(character);
    const int lowercase_character = std::tolower(unsigned_character);
    character = static_cast<char>(lowercase_character);
  }

  return value;
}

// 连接策略
bool shouldCloseConnection(const Request& request)
{
  std::string connection_header;
  const auto connection = request.headers.find("connection");

  if (connection != request.headers.end()) {
    connection_header = lowercase(connection->second);
  }

  const bool explicitly_closes = connection_header == "close";
  const bool explicitly_keeps_alive = connection_header == "keep-alive";
  const bool uses_http_1_0 = request.version == "HTTP/1.0";
  const bool http_1_0_closes = uses_http_1_0 && !explicitly_keeps_alive;

  return explicitly_closes || http_1_0_closes;
}

}  // namespace

// 连接输入处理
ConnectionProcessResult processHttpInput(HttpCodec& codec, muduo::net::Buffer& buffer)
{
  ParseResult parsed = codec.parse(buffer);

  if (parsed.status == ParseStatus::kNeedMoreData) {
    return {true, false, std::nullopt, std::nullopt};
  }

  if (parsed.status == ParseStatus::kComplete) {
    Request request = std::move(parsed.request.value());
    const bool close_connection = shouldCloseConnection(request);
    return {false, close_connection, std::move(request), std::nullopt};
  }

  // 解析失败后无法确定剩余字节边界，因此发送错误响应后必须关闭连接。
  Response response = responseForParseError(parsed.status);
  return {false, true, std::nullopt, std::move(response)};
}

}  // namespace qaiservice::http
