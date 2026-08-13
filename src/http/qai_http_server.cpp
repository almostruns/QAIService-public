#include "http/qai_http_server.h"

#include "chat/chat_routes.h"
#include "chat/chat_conversation_routes.h"
#include "chat/chat_model_config.h"
#include "chat/chat_progress_store.h"
#include "chat/chat_service.h"
#include "assistant/assistant_routes.h"
#include "assistant/proposal_store.h"
#include "http/health.h"
#include "http/http_codec.h"
#include "http/http_connection_handler.h"
#include "http/http_response_writer.h"
#include "http/loop_bound_response_writer.h"
#include "http/middleware.h"
#include "http/web_app.h"
#include "knowledge/document_extractor.h"
#include "knowledge/document_repository.h"
#include "knowledge/file_storage.h"
#include "knowledge/knowledge_routes.h"
#include "knowledge/rerank_client.h"
#include "knowledge/token_client.h"
#include "life/life_routes.h"
#include "persistence/chat_event_consumer.h"
#include "persistence/chat_event_publisher.h"
#include "persistence/chat_message_event.h"
#include "persistence/rabbitmq_config.h"
#include "persistence/rabbitmq_connection.h"
#include "users/auth_middleware.h"
#include "users/auth_routes.h"
#include "users/session_repository.h"
#include "util/time.h"
#include "web_search/baidu_search_provider.h"
#include "web_search/safe_page_fetcher.h"
#include "web_search/search_consent_store.h"
#include "web_search/search_result_selector.h"
#include "web_search/web_cache.h"
#include "web_search/web_evidence_service.h"
#include "web_search/web_page_extractor.h"
#include "web_search/web_search_coordinator.h"

#include "logging/log.h"
#include <muduo/net/Buffer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>

#include <boost/any.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qaiservice::http {
namespace {

// 连接级限制与状态
constexpr std::size_t kMaxPendingInputBytes = 1024 * 1024;

struct HttpConnectionContext {
  HttpCodec codec;
  bool request_in_flight{false};
};

std::filesystem::path fileStorageRoot()
{
  const char* configured = std::getenv("QAI_FILE_STORAGE_ROOT");
  if (configured == nullptr || *configured == '\0') {
    return "/var/lib/qaiservice/files";
  }
  return configured;
}

std::filesystem::path webRoot()
{
  const char* configured = std::getenv("QAI_WEB_ROOT");
  if (configured == nullptr || *configured == '\0') {
    return "/opt/qaiservice/web";
  }
  return configured;
}

}  // namespace

// 生命周期与启动配置
QAIHttpServer::QAIHttpServer(muduo::net::EventLoop* loop, const muduo::net::InetAddress& listen_address,
                             db::DatabaseConfig database_config)
    : worker_pool_("qaiservice-workers"), lifetime_token_(std::make_shared<bool>(true)),
      database_worker_(database_config), sessions_(users::kSessionLifetime),
      file_storage_(std::make_unique<knowledge::FileStorage>(fileStorageRoot())),
      proposals_(std::make_unique<assistant::ProposalStore>(util::currentTimeMilliseconds, 5 * 60 * 1000)),
      server_(loop, listen_address, "qaiservice")
{
  worker_pool_.start(2);

  const persistence::RabbitMqConfig rabbit_config = persistence::rabbitMqConfigFromEnvironment();
  std::unique_ptr<persistence::ChatEventTransport> transport =
      persistence::makeRabbitMqChatEventTransport(rabbit_config);
  chat_event_publisher_ = std::make_unique<persistence::ChatEventPublisher>(std::move(transport));
  chat_event_consumer_ = std::make_unique<persistence::ChatEventConsumer>(rabbit_config, database_config);

  chat::ChatService::TaskExecutor chat_executor = [this](chat::ChatService::Task task) {
    worker_pool_.run(std::move(task));
  };
  chat::ChatService::PersistenceSink persistence_sink = [this](chat::CompletedChatTurn turn) {
    const std::uint64_t user_id = turn.user_id;
    const std::uint64_t first_sequence = turn.first_sequence;
    persistence::ChatMessageCreated user_event{persistence::makeEventId(), turn.user_id, turn.first_sequence,
                                               turn.user_message.role, std::move(turn.user_message.content),
                                               turn.occurred_at_ms, turn.mode, turn.conversation_id};
    persistence::ChatMessageCreated assistant_event{
        persistence::makeEventId(), turn.user_id, turn.first_sequence + 1, turn.assistant_message.role,
        std::move(turn.assistant_message.content), turn.occurred_at_ms, turn.mode, turn.conversation_id};
    std::vector<persistence::ChatMessageCreated> events;
    events.push_back(std::move(user_event));
    events.push_back(std::move(assistant_event));
    const persistence::EnqueueStatus status = chat_event_publisher_->enqueue(std::move(events));
    switch (status) {
      case persistence::EnqueueStatus::kQueued:
        QAI_LOG(info, qaiservice::log::Module::kPersistence) << "chat_events_enqueued user_id=" << user_id << " first_sequence=" << first_sequence
                 << " status=queued";
        return chat::ChatPersistenceStatus::kQueued;
      case persistence::EnqueueStatus::kBusy:
        QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "chat_events_enqueue_failed user_id=" << user_id << " first_sequence=" << first_sequence
                 << " status=busy";
        return chat::ChatPersistenceStatus::kBusy;
      case persistence::EnqueueStatus::kUnavailable:
        QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "chat_events_enqueue_failed user_id=" << user_id << " first_sequence=" << first_sequence
                 << " status=unavailable";
        return chat::ChatPersistenceStatus::kUnavailable;
    }
    return chat::ChatPersistenceStatus::kUnavailable;
  };
  const chat::ChatModelConfig chat_config = chat::chatModelConfigFromEnvironment();
  const char* provider_name = chat_config.provider_kind == chat::ChatProviderKind::kMock ? "mock" : "openai-compatible";
  QAI_LOG(info, qaiservice::log::Module::kChat) << "chat_provider_configured provider=" << provider_name;
  std::unique_ptr<chat::ChatModelProvider> chat_provider = chat::makeChatModelProvider(chat_config);
  chat_service_ = std::make_unique<chat::ChatService>(std::move(chat_provider), std::move(chat_executor), 64, 20,
                                                      32768, std::move(persistence_sink));
  chat_progress_ = std::make_unique<chat::ChatProgressStore>(util::currentTimeMilliseconds, std::chrono::minutes{5});
  const web_search::BaiduSearchConfig web_search_config = web_search::baiduSearchConfigFromEnvironment();
  QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_configured enabled=" << web_search_config.enabled
                         << " provider=baidu timeout_ms=" << web_search_config.timeout.count()
                         << " top_k=" << web_search_config.top_k;
  web_search_provider_ = std::make_unique<web_search::BaiduSearchProvider>(web_search_config);
  web_result_selector_ = std::make_unique<web_search::SearchResultSelector>();
  web_page_fetcher_ = std::make_unique<web_search::SafePageFetcher>(web_search::UrlSafety{},
                                                                    web_search::SafePageFetcherConfig{});
  web_page_extractor_ = std::make_unique<web_search::WebPageExtractor>(12000);
  web_search_cache_ = std::make_unique<web_search::SearchResponseCache>(
      util::currentTimeMilliseconds, 5 * 60 * 1000, 256);
  web_page_cache_ = std::make_unique<web_search::PageBodyCache>(
      util::currentTimeMilliseconds, 10 * 60 * 1000, 256);
  web_evidence_service_ = std::make_unique<web_search::WebEvidenceService>(
      *web_search_provider_, *web_result_selector_, *web_page_fetcher_, *web_page_extractor_, *web_search_cache_,
      *web_page_cache_, web_search::WebEvidenceConfig{});
  web_search_consents_ = std::make_unique<web_search::SearchConsentStore>(
      util::currentTimeMilliseconds, 5 * 60 * 1000, 256);
  std::unique_ptr<chat::ChatModelProvider> planning_model = chat::makeChatModelProvider(chat_config);
  web_search_coordinator_ = std::make_unique<web_search::WebSearchCoordinator>(
      std::move(planning_model), *web_evidence_service_, *web_search_consents_);
  const knowledge::RerankConfig rerank_config = knowledge::rerankConfigFromEnvironment();
  QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "rerank_configured enabled=" << rerank_config.enabled
           << " timeout_ms=" << rerank_config.timeout.count();
  rerank_client_ = std::make_unique<knowledge::RerankClient>(rerank_config);
  const knowledge::TokenConfig token_config = knowledge::tokenConfigFromEnvironment();
  QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "tokenizer_configured enabled=" << token_config.enabled
           << " timeout_ms=" << token_config.timeout.count();
  token_client_ = std::make_unique<knowledge::TokenClient>(token_config);

  registerHealthRoute(router_);
  const users::AuthRouteConfig auth_config = users::authRouteConfigFromEnvironment();
  WebAppConfig web_config;
  web_config.registration_enabled = auth_config.registration_enabled;
  web_config.authenticated = [](const Request& request) {
    return users::authenticatedUserId(request).has_value();
  };
  registerWebAppRoutes(router_, webRoot(), std::move(web_config));
  users::registerAuthRoutes(router_, database_worker_, sessions_, {}, auth_config);
  chat::PersistenceStatusProvider persistence_status_provider = [this] {
    const persistence::PublisherSnapshot publisher = chat_event_publisher_->snapshot();
    const persistence::ConsumerSnapshot consumer = chat_event_consumer_->snapshot();
    return chat::PersistenceRuntimeStatus{publisher.pending, publisher.confirmed, publisher.failed,
                                          consumer.persisted, consumer.duplicates, consumer.rejected,
                                          consumer.connection_failures};
  };
  chat::registerChatRoutes(router_, std::move(persistence_status_provider));
  chat::ChatRouteExecutor chat_route_executor = [this](std::function<void()> task) {
    worker_pool_.run(std::move(task));
  };
  chat::registerChatConversationRoutes(router_, database_worker_, *chat_service_, *chat_progress_,
                                       *web_search_coordinator_, std::move(chat_route_executor));
  knowledge::DocumentSubmissionSink document_sink = [this](std::uint64_t user_id, std::uint64_t document_id) {
    db::DatabaseWorker::Task task = [this, user_id, document_id](db::MySqlConnection& connection) {
      knowledge::DocumentRepository repository(connection);
      const std::optional<knowledge::Document> document = repository.findOwned(user_id, document_id);
      if (!document.has_value()) {
        QAI_LOG(warn, qaiservice::log::Module::kKnowledge) << "document_processing user_id=" << user_id << " document_id=" << document_id
                 << " status=not_found";
        return;
      }
      try {
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_processing user_id=" << user_id << " document_id=" << document_id
                 << " status=processing";
        repository.updateStatus(user_id, document_id, knowledge::DocumentStatus::kProcessing, "");
        knowledge::DocumentExtractor extractor;
        const std::filesystem::path path = file_storage_->pathFor(user_id, document->storage_key);
        const knowledge::ExtractedDocument extracted = extractor.extract(document->media_type, path);
        std::vector<knowledge::NewDocumentChunk> chunks;
        bool tokenized = true;
        for (const knowledge::ExtractedSection& section : extracted.sections) {
          const knowledge::TokenSplitResult split = token_client_->split(section.text, 800, 400);
          if (split.status != knowledge::TokenStatus::kSuccess) {
            tokenized = false;
            break;
          }
          for (const std::string& content : split.chunks) {
            chunks.push_back({static_cast<std::uint32_t>(chunks.size()), content, section.page_number});
          }
        }
        if (!tokenized || chunks.empty()) {
          chunks = knowledge::chunkDocument(extracted);
        }
        repository.replaceChunks(user_id, document_id, chunks);
        repository.updateStatus(user_id, document_id, knowledge::DocumentStatus::kReady, "");
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_processing user_id=" << user_id << " document_id=" << document_id
                 << " status=ready chunks=" << chunks.size() << " tokenizer=" << (tokenized ? "success" : "degraded");
      } catch (const knowledge::DocumentExtractionError&) {
        repository.updateStatus(user_id, document_id, knowledge::DocumentStatus::kFailed, "extraction_failed");
        QAI_LOG(warn, qaiservice::log::Module::kKnowledge) << "document_processing user_id=" << user_id << " document_id=" << document_id
                 << " status=failed reason=extraction_failed";
      } catch (const std::exception& error) {
        repository.updateStatus(user_id, document_id, knowledge::DocumentStatus::kFailed, "processing_failed");
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_processing user_id=" << user_id << " document_id=" << document_id
                  << " status=failed reason=processing_failed error=" << error.what();
      }
    };
    return database_worker_.submit(std::move(task));
  };
  knowledge::registerKnowledgeRoutes(router_, database_worker_, *file_storage_, std::move(document_sink));
  life::registerLifeRoutes(router_, database_worker_);
  assistant::AssistantExecutor assistant_executor = [this](assistant::AssistantTask task) {
    worker_pool_.run(std::move(task));
  };
  assistant::registerAssistantRoutes(router_, database_worker_, *proposals_, *chat_service_, *chat_progress_,
                                     *rerank_client_, *token_client_, *web_search_coordinator_,
                                     std::move(assistant_executor));
  MiddlewareChain middleware;
  middleware.add(makeAccessLogMiddleware([](AccessLogEntry entry) {
    if (entry.path == "/health") {
      QAI_LOG(debug, qaiservice::log::Module::kHttp) << "http_request request_id=" << entry.request_id
                                                     << " method=" << entry.method
                                                     << " path=" << entry.path
                                                     << " status=" << entry.status_code
                                                     << " elapsed_ms=" << entry.elapsed_milliseconds;
      return;
    }
    QAI_LOG(info, qaiservice::log::Module::kHttp) << "http_request request_id=" << entry.request_id << " method=" << entry.method
             << " path=" << entry.path << " status=" << entry.status_code
             << " elapsed_ms=" << entry.elapsed_milliseconds;
  }));
  middleware.add(makeErrorMiddleware());
  middleware.add(users::makeAuthenticationMiddleware(sessions_));
  Handler router_handler = [this](Request request, ResponseWriter writer) {
    router_.route(std::move(request), writer);
  };
  request_handler_ = middleware.compose(std::move(router_handler));

  muduo::net::ConnectionCallback connection_callback = [this](const muduo::net::TcpConnectionPtr& connection) {
    onConnection(connection);
  };
  server_.setConnectionCallback(connection_callback);

  muduo::net::MessageCallback message_callback =
      [this](const muduo::net::TcpConnectionPtr& connection, muduo::net::Buffer* buffer,
             muduo::Timestamp receive_time) {
        onMessage(connection, buffer, receive_time);
      };
      server_.setMessageCallback(message_callback);

  // 必须在 start() 接受连接前完成恢复，否则重启后的首批请求会被误判为未登录。
  loadPersistedSessions();
}

QAIHttpServer::~QAIHttpServer()
{
  QAI_LOG(info, qaiservice::log::Module::kMain) << "service_stopping";
  // 先让已排队的 EventLoop 任务失效，再等待 Worker 停止产生新任务。
  lifetime_token_.reset();
  worker_pool_.stop();
  chat_event_publisher_->stop();
  chat_event_consumer_->stop();
  database_worker_.stop();
  QAI_LOG(info, qaiservice::log::Module::kMain) << "service_stopped";
}

// 服务控制
void QAIHttpServer::start()
{
  server_.start();
  QAI_LOG(info, qaiservice::log::Module::kMain) << "service_listening port=8080";
}

// 会话恢复
void QAIHttpServer::loadPersistedSessions()
{
  std::promise<void> loaded;
  std::future<void> result = loaded.get_future();
  db::DatabaseWorker::Task task = [this, &loaded](db::MySqlConnection& connection) {
    try {
      users::SessionRepository repository(connection);
      const std::int64_t now_ms = util::currentTimeMilliseconds();
      const std::uint64_t purged = repository.eraseExpired(now_ms);
      const std::vector<users::PersistedSession> persisted = repository.loadValid(now_ms);
      for (const users::PersistedSession& session : persisted) {
        const auto expires_at = users::SessionStore::Clock::time_point{std::chrono::milliseconds{session.expires_at_ms}};
        sessions_.loadPersisted(session.token_hash, session.user_id, expires_at);
      }
      QAI_LOG(info, qaiservice::log::Module::kUsers) << "sessions_restored loaded=" << persisted.size() << " purged_expired=" << purged;
      loaded.set_value();
    } catch (...) {
      loaded.set_exception(std::current_exception());
    }
  };

  // 恢复失败则拒绝启动：数据库此刻可用（DatabaseWorker 已就绪），
  // 带着空 session 缓存继续运行会让全部在线用户被静默登出。
  if (!database_worker_.submit(std::move(task))) {
    throw std::runtime_error("cannot schedule session restore");
  }
  result.get();
}

// Muduo 回调
void QAIHttpServer::onConnection(const muduo::net::TcpConnectionPtr& connection)
{
  if (connection->connected()) {
    // 请求可能分成多个 TCP 分片到达，因此解析状态归当前连接所有。
    connection->setContext(HttpConnectionContext{});
  }
}

void QAIHttpServer::onMessage(const muduo::net::TcpConnectionPtr& connection,
                              muduo::net::Buffer* buffer, muduo::Timestamp)
{
  processBufferedInput(connection, *buffer);
}

// 请求处理
void QAIHttpServer::processBufferedInput(const muduo::net::TcpConnectionPtr& connection, muduo::net::Buffer& buffer)
{
  auto* context = boost::any_cast<HttpConnectionContext>(connection->getMutableContext());

  if (context == nullptr) {
    connection->shutdown();
    return;
  }

  if (context->request_in_flight) {
    if (buffer.readableBytes() > kMaxPendingInputBytes) {
      connection->forceClose();
    }
    return;
  }

  if (buffer.readableBytes() == 0) {
    return;
  }

  ConnectionProcessResult result = processHttpInput(context->codec, buffer);

  if (result.needs_more_data) {
    return;
  }

  if (buffer.readableBytes() > kMaxPendingInputBytes) {
    connection->forceClose();
    return;
  }

  context->request_in_flight = true;
  // 等当前响应完成后再继续读，限制慢 Handler 期间的连接级输入增长。
  connection->stopRead();
  ResponseWriter writer = makeResponseWriter(connection, result.close_connection);

  if (result.error_response.has_value()) {
    writer.send(std::move(result.error_response.value()));
    return;
  }

  request_handler_(std::move(result.request.value()), writer);
}

// 安全响应
ResponseWriter QAIHttpServer::makeResponseWriter(const muduo::net::TcpConnectionPtr& connection, bool close_connection)
{
  std::weak_ptr<muduo::net::TcpConnection> weak_connection = connection;
  std::weak_ptr<bool> weak_lifetime = lifetime_token_;
  muduo::net::EventLoop* loop = connection->getLoop();

  LoopScheduler scheduler = [loop](LoopTask task) {
    loop->queueInLoop(std::move(task));
  };
  ConnectionCheck connection_check = [weak_connection, weak_lifetime] {
    if (weak_lifetime.expired()) {
      return false;
    }

    std::shared_ptr<muduo::net::TcpConnection> locked_connection = weak_connection.lock();
    return locked_connection != nullptr && locked_connection->connected();
  };
  ResponseSink response_sink = [this, weak_connection, weak_lifetime, close_connection](Response response) {
    if (weak_lifetime.expired()) {
      return;
    }

    std::shared_ptr<muduo::net::TcpConnection> locked_connection = weak_connection.lock();
    if (locked_connection == nullptr) {
      return;
    }

    locked_connection->getLoop()->assertInLoopThread();
    std::string serialized_response = serializeHttpResponse(response, close_connection);
    locked_connection->send(serialized_response);

    if (close_connection) {
      locked_connection->shutdown();
      return;
    }

    auto* context = boost::any_cast<HttpConnectionContext>(locked_connection->getMutableContext());
    if (context == nullptr) {
      locked_connection->shutdown();
      return;
    }

    context->request_in_flight = false;
    locked_connection->startRead();
    processBufferedInput(locked_connection, *locked_connection->inputBuffer());
  };

  return makeLoopBoundResponseWriter(std::move(scheduler), std::move(connection_check), std::move(response_sink));
}

}  // namespace qaiservice::http
