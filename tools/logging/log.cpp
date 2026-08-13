#include "logging/log.h"

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <cstdlib>
#include <utility>

namespace qaiservice::log {
namespace {

// 日志文件名按日期生成：logs/qaiservice-YYYY-MM-DD.log。
class QaiDailyFilenameCalculator {
 public:
  static spdlog::filename_t calc_filename(const spdlog::filename_t& base_filename,
                                          const std::tm& now_tm)
  {
    std::ostringstream stream;
    stream << base_filename << '-' << std::put_time(&now_tm, "%Y-%m-%d") << ".log";
    return stream.str();
  }
};

}  // namespace

std::string_view moduleName(Module module)
{
  switch (module) {
    case Module::kMain:
      return "main";
    case Module::kHttp:
      return "http";
    case Module::kUsers:
      return "users";
    case Module::kChat:
      return "chat";
    case Module::kDb:
      return "db";
    case Module::kPersistence:
      return "persistence";
    case Module::kKnowledge:
      return "knowledge";
    case Module::kAssistant:
      return "assistant";
    case Module::kLife:
      return "life";
    case Module::kWebSearch:
      return "web_search";
  }
  return "unknown";
}

std::filesystem::path logPathFromEnvironment()
{
  const char* configured_path = std::getenv("QAI_LOG_PATH");
  if (configured_path == nullptr || *configured_path == '\0') {
    return "logs/qaiservice.log";
  }
  return configured_path;
}

void initialize(const std::filesystem::path& log_path)
{
  const std::filesystem::path parent = log_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::filesystem::path base_path = log_path;
  base_path.replace_extension();
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink<std::mutex, QaiDailyFilenameCalculator>>(
      base_path.string(), 0, 0);
  auto logger = std::make_shared<spdlog::logger>("qaiservice",
                                                 spdlog::sinks_init_list{console_sink, file_sink});
  logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%t] [%^%l%$] %v %! (%s:%#)");
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(std::move(logger));
  spdlog::set_level(spdlog::level::info);
}

}  // namespace qaiservice::log
