#include "http/http_codec.h"

#include <muduo/net/Buffer.h>

#include <charconv>
#include <cctype>
#include <string>
#include <utility>

namespace qaiservice::http {
namespace {

// 协议限制
constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxRequestLineBytes = 8 * 1024;
constexpr std::size_t kMaxHeaderBytes = 32 * 1024;

// 文本处理
std::string lowercase(std::string_view input)
{
  std::string output(input);

  for (char& value : output) {
    const unsigned char unsigned_value = static_cast<unsigned char>(value);
    const int lowercase_value = std::tolower(unsigned_value);
    value = static_cast<char>(lowercase_value);
  }

  return output;
}

std::string_view trimOptionalWhitespace(std::string_view value)
{
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }

  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }

  return value;
}

}  // namespace

// 增量解析
ParseResult HttpCodec::parse(muduo::net::Buffer& buffer)
{
  // 尽量消费当前完整数据；TCP 分片不足时保留解析状态。
  while (true) {
    if (state_ == State::kRequestLine) {
      const char* crlf = buffer.findCRLF();

      if (crlf == nullptr) {
        if (buffer.readableBytes() > kMaxRequestLineBytes) {
          state_ = State::kError;
          return {ParseStatus::kBadRequest, std::nullopt};
        }

        return {ParseStatus::kNeedMoreData, std::nullopt};
      }

      const char* request_line_begin = buffer.peek();
      const std::size_t request_line_bytes = static_cast<std::size_t>(crlf - request_line_begin);

      if (request_line_bytes > kMaxRequestLineBytes) {
        state_ = State::kError;
        return {ParseStatus::kBadRequest, std::nullopt};
      }

      const std::string_view request_line(request_line_begin, request_line_bytes);
      if (!parseRequestLine(request_line)) {
        state_ = State::kError;
        return {ParseStatus::kBadRequest, std::nullopt};
      }

      buffer.retrieveUntil(crlf + 2);
      state_ = State::kHeaders;
      continue;
    }

    if (state_ == State::kHeaders) {
      const char* crlf = buffer.findCRLF();

      if (crlf == nullptr) {
        if (header_bytes_ + buffer.readableBytes() > kMaxHeaderBytes) {
          state_ = State::kError;
          return {ParseStatus::kBadRequest, std::nullopt};
        }

        return {ParseStatus::kNeedMoreData, std::nullopt};
      }

      const char* header_line_begin = buffer.peek();
      const std::size_t header_content_bytes = static_cast<std::size_t>(crlf - header_line_begin);
      const std::size_t line_bytes = header_content_bytes + 2;

      if (header_bytes_ + line_bytes > kMaxHeaderBytes) {
        state_ = State::kError;
        return {ParseStatus::kBadRequest, std::nullopt};
      }

      header_bytes_ += line_bytes;

      if (crlf != header_line_begin) {
        const std::string_view header_line(header_line_begin, header_content_bytes);
        if (!parseHeader(header_line)) {
          state_ = State::kError;
          return {ParseStatus::kBadRequest, std::nullopt};
        }

        buffer.retrieveUntil(crlf + 2);
        continue;
      }

      buffer.retrieveUntil(crlf + 2);

      if (request_.headers.count("transfer-encoding") != 0) {
        state_ = State::kError;
        return {ParseStatus::kNotImplemented, std::nullopt};
      }

      const auto content_length = request_.headers.find("content-length");

      if (content_length != request_.headers.end()) {
        const std::string& value = content_length->second;
        const char* value_end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), value_end, content_length_);

        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value_end) {
          state_ = State::kError;
          return {ParseStatus::kBadRequest, std::nullopt};
        }

        if (content_length_ > kMaxBodyBytes) {
          state_ = State::kError;
          return {ParseStatus::kPayloadTooLarge, std::nullopt};
        }
      }

      if (content_length_ > 0) {
        state_ = State::kBody;
        continue;
      }

      Request completed = std::move(request_);
      reset();
      return {ParseStatus::kComplete, std::move(completed)};
    }

    if (state_ == State::kBody) {
      if (buffer.readableBytes() < content_length_) {
        return {ParseStatus::kNeedMoreData, std::nullopt};
      }

      request_.body.assign(buffer.peek(), content_length_);
      buffer.retrieve(content_length_);

      Request completed = std::move(request_);
      reset();
      return {ParseStatus::kComplete, std::move(completed)};
    }

    return {ParseStatus::kBadRequest, std::nullopt};
  }
}

// 请求行解析
bool HttpCodec::parseRequestLine(std::string_view line)
{
  const std::size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos || first_space == 0) {
    return false;
  }

  const std::size_t second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos || second_space == first_space + 1) {
    return false;
  }

  const std::string_view version = line.substr(second_space + 1);
  if (version != "HTTP/1.0" && version != "HTTP/1.1") {
    return false;
  }

  request_.method.assign(line.substr(0, first_space));
  request_.version.assign(version);

  const std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
  const std::size_t query = target.find('?');
  request_.path.assign(target.substr(0, query));
  if (query != std::string_view::npos) {
    request_.query.assign(target.substr(query + 1));
  }

  if (request_.path.empty()) {
    return false;
  }

  return request_.path.front() == '/';
}

// Header 解析
bool HttpCodec::parseHeader(std::string_view line)
{
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return false;
  }

  std::string name = lowercase(line.substr(0, colon));
  if (name == "content-length" && request_.headers.count(name) != 0) {
    return false;
  }

  std::string value(trimOptionalWhitespace(line.substr(colon + 1)));

  request_.headers.insert_or_assign(std::move(name), std::move(value));
  return true;
}

// 状态重置
void HttpCodec::reset()
{
  // 同一个 Buffer 中可能紧接着还有下一个请求。
  state_ = State::kRequestLine;
  request_ = Request{};
  content_length_ = 0;
  header_bytes_ = 0;
}

}  // namespace qaiservice::http
