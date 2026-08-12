#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace qaiservice::util {

inline std::size_t utf8CodePointBytes(unsigned char first)
{
  if ((first & 0x80) == 0) {
    return 1;
  }
  if ((first & 0xe0) == 0xc0) {
    return 2;
  }
  if ((first & 0xf0) == 0xe0) {
    return 3;
  }
  if ((first & 0xf8) == 0xf0) {
    return 4;
  }
  return 1;
}

inline std::size_t utf8Boundary(std::string_view text, std::size_t limit)
{
  std::size_t boundary = std::min(limit, text.size());
  while (boundary > 0 && boundary < text.size() &&
         (static_cast<unsigned char>(text[boundary]) & 0xc0) == 0x80) {
    --boundary;
  }
  return boundary == 0 ? std::min(limit, text.size()) : boundary;
}

}  // namespace qaiservice::util
