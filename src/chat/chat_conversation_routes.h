#pragma once

#include <functional>

namespace qaiservice::db {
class DatabaseWorker;
}

namespace qaiservice::http {
class Router;
}

namespace qaiservice::chat {

class ChatService;
class ChatProgressStore;

using ChatRouteExecutor = std::function<void(std::function<void()>)>;

}

namespace qaiservice::web_search {
class WebSearchCoordinator;
}

namespace qaiservice::chat {

void registerChatConversationRoutes(http::Router& router, db::DatabaseWorker& database_worker,
                                    ChatService& chat_service, ChatProgressStore& progress_store,
                                    web_search::WebSearchCoordinator& web_search, ChatRouteExecutor executor);

}  // namespace qaiservice::chat
