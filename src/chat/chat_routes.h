#pragma once

#include <cstddef>
#include <functional>

namespace qaiservice::http {
class Router;
}

namespace qaiservice::chat {

struct PersistenceRuntimeStatus {
  std::size_t pending;
  std::size_t confirmed;
  std::size_t failed;
  std::size_t persisted;
  std::size_t duplicates;
  std::size_t rejected;
  std::size_t connection_failures;
};

using PersistenceStatusProvider = std::function<PersistenceRuntimeStatus()>;

void registerChatRoutes(http::Router& router, PersistenceStatusProvider persistence_status_provider = {});

}  // namespace qaiservice::chat
