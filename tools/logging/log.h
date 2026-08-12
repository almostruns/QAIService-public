#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <sstream>
#include <string_view>

namespace qaiservice::log {

// 初始化日志：控制台 + 按日期命名文件（logs/qaiservice-YYYY-MM-DD.log，每天一个）。
void initialize(const std::filesystem::path& log_path);
[[nodiscard]] std::filesystem::path logPathFromEnvironment();

// 流式日志对象：析构时以 source_loc（文件/行/函数）和模块前缀落一条日志。
class LogStream {
 public:
  LogStream(spdlog::source_loc location, spdlog::level::level_enum level, std::string_view module)
      : location_(location), level_(level), module_(module)
  {
  }

  ~LogStream()
  {
    spdlog::default_logger_raw()->log(location_, level_, "[{}] {}", module_, buffer_.str());
  }

  template <typename Value>
  LogStream& operator<<(const Value& value)
  {
    buffer_ << value;
    return *this;
  }

 private:
  spdlog::source_loc location_;
  spdlog::level::level_enum level_;
  std::string_view module_;
  std::ostringstream buffer_;
};

}  // namespace qaiservice::log

// 统一日志入口：QAI_LOG(info, "chat") << "message key=value";
// 输出格式：时间 [线程] [级别] [模块] 消息 函数 (文件:行)
#define QAI_LOG(level_enum, module)                                                          \
  ::qaiservice::log::LogStream(::spdlog::source_loc{__FILE__, __LINE__, __func__},          \
                               ::spdlog::level::level_enum, module)
