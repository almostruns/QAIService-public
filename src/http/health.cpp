#include "http/health.h"

#include "http/router.h"

#include <utility>

namespace qaiservice::http {

// 路由注册
void registerHealthRoute(Router& router)
{
  Handler health_handler = [](Request, ResponseWriter writer) {
    writer.send(Response{200, "application/json", R"({"status":"ok"})"});
  };

  router.add("GET", "/health", std::move(health_handler));
}

}  // namespace qaiservice::http
