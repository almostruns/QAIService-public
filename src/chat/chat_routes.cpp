#include "chat/chat_routes.h"

#include "chat/chat_service.h"
#include "http/json_response.h"
#include "http/router.h"
#include "users/auth_middleware.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace qaiservice::chat {

void registerChatRoutes(http::Router& router, PersistenceStatusProvider persistence_status_provider)
{
  router.add("GET", "/api/chat/persistence",
             [persistence_status_provider = std::move(persistence_status_provider)](
                 http::Request request, http::ResponseWriter writer) {
    if (!users::authenticatedUserId(request).has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!persistence_status_provider) {
      writer.send(http::jsonResponse(503, {{"error", "persistence_status_unavailable"}}));
      return;
    }

    const PersistenceRuntimeStatus status = persistence_status_provider();
    writer.send(http::jsonResponse(200, {{"publisher", {{"pending", status.pending},
                                                   {"confirmed", status.confirmed},
                                                   {"failed", status.failed}}},
                                   {"consumer", {{"persisted", status.persisted},
                                                  {"duplicates", status.duplicates},
                                                  {"rejected", status.rejected},
                                                  {"connection_failures", status.connection_failures}}}}));
  });
}

}  // namespace qaiservice::chat
