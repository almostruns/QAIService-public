#pragma once

#include <functional>

namespace qaiservice::db {
class DatabaseWorker;
}

namespace qaiservice::http {
class Router;
}

namespace qaiservice::chat {
class ChatProgressStore;
class ChatService;
}

namespace qaiservice::knowledge {
class RerankClient;
class TokenClient;
}

namespace qaiservice::web_search {
class WebSearchCoordinator;
}

namespace qaiservice::assistant {

class ProposalStore;

using AssistantTask = std::function<void()>;
using AssistantExecutor = std::function<void(AssistantTask)>;

void registerAssistantRoutes(http::Router& router, db::DatabaseWorker& database_worker, ProposalStore& proposals,
                             chat::ChatService& chat_service, chat::ChatProgressStore& progress_store,
                             knowledge::RerankClient& rerank_client, knowledge::TokenClient& token_client,
                             web_search::WebSearchCoordinator& web_search, AssistantExecutor assistant_executor);

}  // namespace qaiservice::assistant
