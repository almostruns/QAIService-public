#pragma once

#include "http/http_types.h"

#include <functional>

namespace qaiservice::http {

// EventLoop 调度
using LoopTask = std::function<void()>;
using LoopScheduler = std::function<void(LoopTask)>;

// 连接检查与响应输出
using ConnectionCheck = std::function<bool()>;
using ResponseSink = std::function<void(Response)>;

// Writer 构造
[[nodiscard]] ResponseWriter makeLoopBoundResponseWriter(LoopScheduler scheduler,
                                                         ConnectionCheck connection_check,
                                                         ResponseSink response_sink);

}  // namespace qaiservice::http
