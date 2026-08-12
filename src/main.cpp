#include "db/database_config.h"
#include "http/qai_http_server.h"

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include "logging/log.h"

#include <filesystem>
#include <utility>

int main()
{
  const std::filesystem::path log_path = qaiservice::log::logPathFromEnvironment();
  qaiservice::log::initialize(log_path);
  QAI_LOG(info, "main") << "service_start log_path=" << log_path.string();

  // 服务配置
  constexpr unsigned short kPort = 8080;

  // 网络运行时
  muduo::net::EventLoop loop;
  const muduo::net::InetAddress listen_address(kPort);
  qaiservice::db::DatabaseConfig database_config = qaiservice::db::databaseConfigFromEnvironment();
  qaiservice::http::QAIHttpServer server(&loop, listen_address, std::move(database_config));

  server.start();
  loop.loop();
}
