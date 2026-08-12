#include "http/http_response_writer.h"

#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>

namespace qaiservice::http {
namespace {

// 状态行映射
std::string statusLine(int status_code)
{
  switch (status_code) {
    case 200:
      return "200 OK";
    case 201:
      return "201 Created";
    case 204:
      return "204 No Content";
    case 400:
      return "400 Bad Request";
    case 401:
      return "401 Unauthorized";
    case 403:
      return "403 Forbidden";
    case 404:
      return "404 Not Found";
    case 409:
      return "409 Conflict";
    case 413:
      return "413 Payload Too Large";
    case 422:
      return "422 Unprocessable Content";
    case 429:
      return "429 Too Many Requests";
    case 500:
      return "500 Internal Server Error";
    case 501:
      return "501 Not Implemented";
    case 502:
      return "502 Bad Gateway";
    case 503:
      return "503 Service Unavailable";
    case 504:
      return "504 Gateway Timeout";
    default:
      break;
  }

  if (status_code >= 100 && status_code <= 599) {
    return std::to_string(status_code) + " Unknown";
  }

  // Handler 给出非法状态时稳定降级，不能让异常逃出 EventLoop。
  return "500 Internal Server Error";
}

}  // namespace

// 一次性发送状态
struct ResponseWriter::State {
  explicit State(SendFunction function) : send_function(std::move(function))
  {
  }

  SendFunction send_function;
  std::atomic_bool response_sent{false};
};

// ResponseWriter
ResponseWriter::ResponseWriter(SendFunction send_function) : state_(std::make_shared<State>(std::move(send_function)))
{
}

void ResponseWriter::send(Response response) const
{
  bool expected = false;
  if (!state_->response_sent.compare_exchange_strong(expected, true)) {
    return;
  }

  state_->send_function(std::move(response));
}

// HTTP 响应序列化
std::string serializeHttpResponse(const Response& response, bool close_connection)
{
  std::string output;
  output.reserve(response.body.size() + 128);

  output += "HTTP/1.1 ";
  output += statusLine(response.status_code);

  output += "\r\nContent-Type: ";
  output += response.content_type;

  for (const auto& [name, value] : response.headers) {
    output += "\r\n";
    output += name;
    output += ": ";
    output += value;
  }

  output += "\r\nContent-Length: ";
  output += std::to_string(response.body.size());

  output += "\r\nConnection: ";
  output += close_connection ? "close" : "keep-alive";

  output += "\r\n\r\n";
  output += response.body;

  return output;
}

// 解析错误映射
Response responseForParseError(ParseStatus status)
{
  switch (status) {
    case ParseStatus::kBadRequest:
      return {400, "application/json", R"({"error":"bad_request"})"};
    case ParseStatus::kPayloadTooLarge:
      return {413, "application/json", R"({"error":"payload_too_large"})"};
    case ParseStatus::kNotImplemented:
      return {501, "application/json", R"({"error":"transfer_encoding_not_supported"})"};
    case ParseStatus::kNeedMoreData:
    case ParseStatus::kComplete:
      throw std::invalid_argument("parse status is not an error");
  }
  throw std::invalid_argument("unknown parse status");
}

}  // namespace qaiservice::http
