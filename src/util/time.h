#pragma once

#include <chrono>
#include <cstdint>

namespace qaiservice::util {

inline std::int64_t currentTimeMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace qaiservice::util
