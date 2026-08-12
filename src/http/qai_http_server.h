#pragma once

#include "db/database_worker.h"
#include "http/router.h"
#include "users/session_store.h"

#include <muduo/base/ThreadPool.h>
#include <muduo/net/TcpServer.h>

#include <memory>

namespace qaiservice::chat {
class ChatService;
class ChatProgressStore;
}

namespace qaiservice::persistence {
class ChatEventConsumer;
class ChatEventPublisher;
}

namespace qaiservice::knowledge {
class FileStorage;
class RerankClient;
class TokenClient;
}

namespace qaiservice::assistant {
class ProposalStore;
}

namespace qaiservice::web_search {
class PageBodyCache;
class SafePageFetcher;
class SearchConsentStore;
class SearchProvider;
class SearchResponseCache;
class SearchResultSelector;
class WebEvidenceService;
class WebPageExtractor;
class WebSearchCoordinator;
}

namespace muduo::net {
class Buffer;
}  // namespace muduo::net

namespace qaiservice::http {

// 连接 Muduo TCP 回调与可脱离 socket 测试的 HTTP 处理层。
// Server 必须在所属 EventLoop 线程销毁，不能与该线程的回调并发析构。
class QAIHttpServer {
 public:
  // 生命周期
  QAIHttpServer(muduo::net::EventLoop* loop, const muduo::net::InetAddress& listen_address,
                db::DatabaseConfig database_config);
  ~QAIHttpServer();

  // 服务控制
  void start();

 private:
  // Muduo 回调
  void onConnection(const muduo::net::TcpConnectionPtr& connection);
  void onMessage(const muduo::net::TcpConnectionPtr& connection, muduo::net::Buffer* buffer,
                 muduo::Timestamp receive_time);

  // 请求处理
  void processBufferedInput(const muduo::net::TcpConnectionPtr& connection, muduo::net::Buffer& buffer);
  [[nodiscard]] ResponseWriter makeResponseWriter(const muduo::net::TcpConnectionPtr& connection,
                                                  bool close_connection);

  // 会话恢复
  void loadPersistedSessions();

  // 工作线程与生命周期
  muduo::ThreadPool worker_pool_;
  std::shared_ptr<bool> lifetime_token_;
  std::unique_ptr<persistence::ChatEventPublisher> chat_event_publisher_;
  std::unique_ptr<persistence::ChatEventConsumer> chat_event_consumer_;
  std::unique_ptr<chat::ChatService> chat_service_;
  std::unique_ptr<chat::ChatProgressStore> chat_progress_;
  db::DatabaseWorker database_worker_;
  users::SessionStore sessions_;
  std::unique_ptr<knowledge::FileStorage> file_storage_;
  std::unique_ptr<knowledge::RerankClient> rerank_client_;
  std::unique_ptr<knowledge::TokenClient> token_client_;
  std::unique_ptr<assistant::ProposalStore> proposals_;
  std::unique_ptr<web_search::SearchProvider> web_search_provider_;
  std::unique_ptr<web_search::SearchResultSelector> web_result_selector_;
  std::unique_ptr<web_search::SafePageFetcher> web_page_fetcher_;
  std::unique_ptr<web_search::WebPageExtractor> web_page_extractor_;
  std::unique_ptr<web_search::SearchResponseCache> web_search_cache_;
  std::unique_ptr<web_search::PageBodyCache> web_page_cache_;
  std::unique_ptr<web_search::WebEvidenceService> web_evidence_service_;
  std::unique_ptr<web_search::SearchConsentStore> web_search_consents_;
  std::unique_ptr<web_search::WebSearchCoordinator> web_search_coordinator_;

  // HTTP 处理
  Router router_;
  Handler request_handler_;

  // 网络服务
  muduo::net::TcpServer server_;
};

}  // namespace qaiservice::http
