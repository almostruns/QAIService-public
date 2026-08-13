#include "chat/chat_conversation_routes.h"

#include "chat/chat_names.h"
#include "chat/chat_progress_store.h"
#include "chat/chat_service.h"
#include "chat/conversation_title.h"
#include "db/database_worker.h"
#include "http/json_response.h"
#include "http/router.h"
#include "persistence/chat_conversation_repository.h"
#include "persistence/chat_message_repository.h"
#include "users/auth_middleware.h"
#include "util/time.h"
#include "web_search/web_answer_envelope.h"
#include "web_search/web_search_coordinator.h"

#include "logging/log.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qaiservice::chat {
namespace {

constexpr std::size_t kMaximumPromptBytes = 4096;

std::string internalRequestId()
{
  static std::atomic<std::uint64_t> next_id{1};
  return "general-internal-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
}

struct ChatRequest {
  std::uint64_t conversation_id;
  std::string message;
  std::optional<std::string> request_id;
  bool web_search_enabled;
  std::optional<std::string> web_search_consent_token;
};

std::optional<ConversationMode> modeFromQuery(const std::string& query)
{
  constexpr char prefix[] = "mode=";
  if (query.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  return chat::parseConversationMode(query.substr(sizeof(prefix) - 1));
}

std::optional<std::uint64_t> pathId(const http::Request& request)
{
  const auto parameter = request.path_parameters.find("conversation_id");
  if (parameter == request.path_parameters.end()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const std::string& text = parameter->second;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<ConversationMode> modeFromBody(const http::Request& request)
{
  try {
    const nlohmann::json body = nlohmann::json::parse(request.body);
    if (!body.is_object() || !body.contains("mode") || !body["mode"].is_string()) {
      return std::nullopt;
    }
    return chat::parseConversationMode(body["mode"].get<std::string>());
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

std::optional<ChatRequest> parseChatRequest(const http::Request& request)
{
  try {
    const nlohmann::json body = nlohmann::json::parse(request.body);
    if (!body.is_object() || !body.contains("conversation_id") ||
        !body["conversation_id"].is_number_unsigned() || !body.contains("message") ||
        !body["message"].is_string()) {
      return std::nullopt;
    }
    std::optional<std::string> request_id;
    if (body.contains("request_id")) {
      if (!body["request_id"].is_string()) {
        return std::nullopt;
      }
      const std::string value = body["request_id"].get<std::string>();
      if (value.empty() || value.size() > 128) {
        return std::nullopt;
      }
      request_id = value;
    }
    std::optional<std::string> consent_token;
    if (body.contains("web_search_consent_token")) {
      if (!body["web_search_consent_token"].is_string()) {
        return std::nullopt;
      }
      const std::string value = body["web_search_consent_token"].get<std::string>();
      if (value.empty() || value.size() > 128) {
        return std::nullopt;
      }
      consent_token = value;
    }
    bool web_search_enabled = false;
    if (body.contains("web_search_enabled")) {
      if (!body["web_search_enabled"].is_boolean()) {
        return std::nullopt;
      }
      web_search_enabled = body["web_search_enabled"].get<bool>();
    }
    ChatRequest parsed{body["conversation_id"].get<std::uint64_t>(), body["message"].get<std::string>(),
                       std::move(request_id), web_search_enabled, std::move(consent_token)};
    if (parsed.conversation_id == 0 || parsed.message.empty()) {
      return std::nullopt;
    }
    return parsed;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> progressRequestId(const http::Request& request)
{
  const auto parameter = request.path_parameters.find("request_id");
  if (parameter == request.path_parameters.end() || parameter->second.empty() || parameter->second.size() > 128) {
    return std::nullopt;
  }
  return parameter->second;
}

std::vector<ChatProgressStage> generalProgressStages()
{
  return {{"decide", "判断是否需要联网"},
          {"privacy", "检查查询隐私"},
          {"search", "搜索并筛选来源"},
          {"fetch", "获取网页正文"},
          {"evidence", "整理可信证据"},
          {"model", "基于证据思考"},
          {"persist", "保存回答"},
          {"complete", "完成"}};
}

const char* progressStatusName(ChatProgressStatus status)
{
  if (status == ChatProgressStatus::kCompleted) {
    return "completed";
  }
  if (status == ChatProgressStatus::kFailed) {
    return "failed";
  }
  return "running";
}

const char* progressStageStatusName(ChatProgressStageStatus status)
{
  if (status == ChatProgressStageStatus::kRunning) {
    return "running";
  }
  if (status == ChatProgressStageStatus::kCompleted) {
    return "completed";
  }
  if (status == ChatProgressStageStatus::kSkipped) {
    return "skipped";
  }
  if (status == ChatProgressStageStatus::kFailed) {
    return "failed";
  }
  return "pending";
}

nlohmann::json progressJson(const ChatProgressSnapshot& progress)
{
  nlohmann::json stages = nlohmann::json::array();
  for (const ChatProgressStageSnapshot& stage : progress.stages) {
    const nlohmann::json stage_json = {
        {"code", stage.code}, {"label", stage.label}, {"status", progressStageStatusName(stage.status)}};
    stages.push_back(stage_json);
  }
  return {{"conversation_id", progress.conversation_id},
          {"status", progressStatusName(progress.status)},
          {"stage_index", progress.stage_index},
          {"stage_count", progress.stage_count},
          {"stage_code", progress.stage.code},
          {"stage_label", progress.stage.label},
          {"stages", std::move(stages)},
          {"updated_at_ms", progress.updated_at_ms}};
}

void failProgress(ChatProgressStore& store, std::uint64_t user_id, const std::optional<std::string>& request_id)
{
  if (request_id.has_value()) {
    const bool failed = store.fail(user_id, request_id.value());
    (void)failed;
  }
}

nlohmann::json conversationJson(const persistence::ChatConversation& conversation)
{
  return {{"id", conversation.id},
          {"mode", chat::conversationModeName(conversation.mode)},
          {"title", conversation.title},
          {"created_at_ms", conversation.created_at_ms},
          {"updated_at_ms", conversation.updated_at_ms}};
}

std::string visibleContent(ConversationMode mode, ChatRole role, const std::string& content)
{
  if (role != ChatRole::kAssistant) {
    return content;
  }
  const web_search::WebAnswerEnvelope web_answer = web_search::parseWebAnswer(content);
  if (!web_answer.web_sources.empty()) {
    return web_answer.message;
  }
  if (mode != ConversationMode::kPrivate) {
    const auto body = nlohmann::json::parse(content, nullptr, false);
    if (!body.is_discarded() && body.is_object() && body.contains("message") && body["message"].is_string() &&
        body.contains("web_sources") && body["web_sources"].is_array()) {
      return body["message"].get<std::string>();
    }
    return content;
  }
  try {
    const nlohmann::json body = nlohmann::json::parse(content);
    if (body.is_object() && body.contains("message") && body["message"].is_string()) {
      return body["message"].get<std::string>();
    }
  } catch (const nlohmann::json::exception&) {
  }
  return content;
}

nlohmann::json webSourcesJson(const std::vector<web_search::WebSource>& sources)
{
  nlohmann::json result = nlohmann::json::array();
  for (const web_search::WebSource& source : sources) {
    result.push_back({{"id", source.id},
                      {"title", source.title},
                      {"url", source.url},
                      {"site", source.site},
                      {"published_at", source.published_at}});
  }
  return result;
}

http::Response completionResponse(const ChatCompletion& completion)
{
  if (completion.status == ChatCompletionStatus::kSuccess) {
    const char* persistence = "not_configured";
    if (completion.persistence == ChatPersistenceStatus::kQueued) {
      persistence = "queued";
    } else if (completion.persistence == ChatPersistenceStatus::kBusy) {
      persistence = "busy";
    } else if (completion.persistence == ChatPersistenceStatus::kUnavailable) {
      persistence = "unavailable";
    }
    const web_search::WebAnswerEnvelope answer = web_search::parseWebAnswer(completion.message.content);
    return http::jsonResponse(200, {{"message", answer.message},
                                    {"web_sources", webSourcesJson(answer.web_sources)},
                                    {"persistence", persistence}});
  }
  if (completion.status == ChatCompletionStatus::kInvalidRequest) {
    return http::jsonResponse(400, {{"error", "model_invalid_request"}});
  }
  if (completion.status == ChatCompletionStatus::kTimeout) {
    return http::jsonResponse(504, {{"error", "model_timeout"}});
  }
  if (completion.status == ChatCompletionStatus::kUnavailable) {
    return http::jsonResponse(503, {{"error", "model_unavailable"}});
  }
  if (completion.status == ChatCompletionStatus::kUpstreamError) {
    return http::jsonResponse(502, {{"error", "model_upstream_error"}});
  }
  return http::jsonResponse(502, {{"error", "model_invalid_response"}});
}

http::Response webPreparationResponse(const web_search::WebPreparation& preparation)
{
  if (preparation.status == web_search::WebPreparationStatus::kConsentRequired) {
    return http::jsonResponse(409, {{"error", "web_search_consent_required"},
                                    {"consent_token", preparation.consent_token},
                                    {"outbound_queries", preparation.outbound_queries},
                                    {"risk_types", preparation.risk_types}});
  }
  if (preparation.status == web_search::WebPreparationStatus::kInvalidConsent) {
    return http::jsonResponse(409, {{"error", "web_search_consent_invalid_or_expired"}});
  }
  if (preparation.status == web_search::WebPreparationStatus::kPlannerUnavailable) {
    return http::jsonResponse(503, {{"error", "web_search_planner_unavailable"}});
  }
  if (preparation.evidence_status == web_search::WebEvidenceStatus::kRateLimited) {
    return http::jsonResponse(429, {{"error", "web_search_rate_limited"}});
  }
  if (preparation.evidence_status == web_search::WebEvidenceStatus::kQuotaExhausted) {
    return http::jsonResponse(503, {{"error", "web_search_quota_exhausted"}});
  }
  if (preparation.evidence_status == web_search::WebEvidenceStatus::kTimeout) {
    return http::jsonResponse(504, {{"error", "web_search_timeout"}});
  }
  if (preparation.evidence_status == web_search::WebEvidenceStatus::kNotConfigured) {
    return http::jsonResponse(503, {{"error", "web_search_not_configured"}});
  }
  if (preparation.evidence_status == web_search::WebEvidenceStatus::kUnauthorized) {
    return http::jsonResponse(503, {{"error", "web_search_unauthorized"}});
  }
  return http::jsonResponse(502, {{"error", "web_search_unavailable"}});
}

void restoreConversation(ChatService& service, persistence::ChatMessageRepository& messages,
                         const persistence::ChatConversation& conversation)
{
  const std::vector<persistence::ChatMessageCreated> stored =
      messages.findByConversationOrdered(conversation.user_id, conversation.id);
  std::vector<ChatMessage> history;
  history.reserve(stored.size());
  std::uint64_t next_sequence = 1;
  for (const persistence::ChatMessageCreated& message : stored) {
    history.push_back({message.role, message.content});
    next_sequence = std::max(next_sequence, message.sequence + 1);
  }
  const bool restored = service.restoreIfUninitialized(
      conversation.user_id, conversation.mode, conversation.id, std::move(history), next_sequence);
  (void)restored;
}

}  // namespace

void registerChatConversationRoutes(http::Router& router, db::DatabaseWorker& database_worker,
                                    ChatService& chat_service, ChatProgressStore& progress_store,
                                    web_search::WebSearchCoordinator& web_search, ChatRouteExecutor executor)
{
  router.add("GET", "/api/chat/progress/conversation/{conversation_id}",
             [&progress_store](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> conversation_id = pathId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!conversation_id.has_value()) {
      writer.send(http::jsonResponse(404, {{"error", "chat_progress_not_found"}}));
      return;
    }
    const std::optional<ChatProgressLookup> lookup =
        progress_store.findLatestByConversation(user_id.value(), conversation_id.value());
    if (!lookup.has_value()) {
      writer.send(http::jsonResponse(404, {{"error", "chat_progress_not_found"}}));
      return;
    }
    const ChatProgressSnapshot& progress = lookup->progress;
    nlohmann::json body = progressJson(progress);
    body["request_id"] = lookup->request_id;
    writer.send(http::jsonResponse(200, body));
  });

  router.add("GET", "/api/chat/progress/{request_id}",
             [&progress_store](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::string> request_id = progressRequestId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!request_id.has_value()) {
      writer.send(http::jsonResponse(404, {{"error", "chat_progress_not_found"}}));
      return;
    }
    const std::optional<ChatProgressSnapshot> progress = progress_store.findOwned(user_id.value(), request_id.value());
    if (!progress.has_value()) {
      writer.send(http::jsonResponse(404, {{"error", "chat_progress_not_found"}}));
      return;
    }
    writer.send(http::jsonResponse(200, progressJson(progress.value())));
  });

  router.add("POST", "/api/conversations",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<ConversationMode> mode = modeFromBody(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!mode.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_conversation_mode"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), mode = mode.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        persistence::ChatConversationRepository repository(connection);
        const persistence::ChatConversation conversation = repository.create(user_id, mode, util::currentTimeMilliseconds());
        QAI_LOG(info, qaiservice::log::Module::kChat) << "conversation_created user_id=" << user_id
                                                       << " conversation_id=" << conversation.id
                                                       << " mode=" << static_cast<int>(mode);
        writer.send(http::jsonResponse(201, {{"conversation", conversationJson(conversation)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kChat) << "conversation_create user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "conversation_create_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });

  router.add("GET", "/api/conversations",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<ConversationMode> mode = modeFromQuery(request.query);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!mode.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_conversation_mode"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), mode = mode.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        persistence::ChatConversationRepository repository(connection);
        nlohmann::json conversations = nlohmann::json::array();
        for (const persistence::ChatConversation& conversation : repository.listOwned(user_id, mode)) {
          conversations.push_back(conversationJson(conversation));
        }
        QAI_LOG(info, qaiservice::log::Module::kChat) << "conversations_listed user_id=" << user_id
                                                       << " mode=" << static_cast<int>(mode)
                                                       << " count=" << conversations.size();
        writer.send(http::jsonResponse(200, {{"conversations", std::move(conversations)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kChat) << "conversation_list user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "conversation_list_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });

  router.add("GET", "/api/conversations/{conversation_id}/messages",
             [&database_worker, &chat_service](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> conversation_id = pathId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!conversation_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_conversation_id"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), conversation_id = conversation_id.value(),
                                     writer, &chat_service](db::MySqlConnection& connection) {
      try {
        persistence::ChatConversationRepository conversations(connection);
        const std::optional<persistence::ChatConversation> conversation =
            conversations.findOwned(user_id, conversation_id);
        if (!conversation.has_value()) {
          writer.send(http::jsonResponse(404, {{"error", "conversation_not_found"}}));
          return;
        }
        persistence::ChatMessageRepository messages(connection);
        restoreConversation(chat_service, messages, conversation.value());
        nlohmann::json history = nlohmann::json::array();
        for (const ChatMessage& message :
             chat_service.history(user_id, conversation->mode, conversation_id)) {
          nlohmann::json entry{{"role", chat::chatRoleName(message.role)},
                               {"content", visibleContent(conversation->mode, message.role, message.content)}};
          if (message.role == ChatRole::kAssistant) {
            const web_search::WebAnswerEnvelope answer = web_search::parseWebAnswer(message.content);
            entry["web_sources"] = webSourcesJson(answer.web_sources);
          }
          history.push_back(std::move(entry));
        }
        QAI_LOG(info, qaiservice::log::Module::kChat) << "conversation_history_loaded user_id=" << user_id
                                                       << " conversation_id=" << conversation_id
                                                       << " message_count=" << history.size();
        writer.send(http::jsonResponse(200, {{"conversation", conversationJson(conversation.value())},
                                       {"messages", std::move(history)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kChat) << "conversation_history user_id=" << user_id << " conversation_id=" << conversation_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "conversation_history_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });

  router.add("DELETE", "/api/conversations/{conversation_id}",
             [&database_worker, &chat_service, &progress_store](http::Request request,
                                                                http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> conversation_id = pathId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!conversation_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_conversation_id"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), conversation_id = conversation_id.value(),
                                     writer, &chat_service, &progress_store](db::MySqlConnection& connection) {
      try {
        persistence::ChatConversationRepository repository(connection);
        const std::optional<persistence::ChatConversation> conversation =
            repository.findOwned(user_id, conversation_id);
        if (!conversation.has_value()) {
          writer.send(http::jsonResponse(404, {{"error", "conversation_not_found"}}));
          return;
        }
        const auto active_progress = progress_store.findLatestByConversation(user_id, conversation_id);
        if (active_progress.has_value() &&
            active_progress->progress.status == ChatProgressStatus::kRunning) {
          writer.send(http::jsonResponse(409, {{"error", "conversation_busy"}}));
          return;
        }
        if (chat_service.eraseIfIdle(user_id, conversation->mode, conversation_id) ==
            ConversationEraseStatus::kBusy) {
          writer.send(http::jsonResponse(409, {{"error", "conversation_busy"}}));
          return;
        }
        if (!repository.removeOwned(user_id, conversation_id)) {
          writer.send(http::jsonResponse(404, {{"error", "conversation_not_found"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kChat) << "conversation_deleted user_id=" << user_id
                                                       << " conversation_id=" << conversation_id;
        writer.send(http::jsonResponse(200, {{"deleted", true}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kChat) << "conversation_delete user_id=" << user_id << " conversation_id=" << conversation_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "conversation_delete_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });

  router.add("POST", "/api/chat",
             [&database_worker, &chat_service, &progress_store, &web_search,
              executor](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    std::optional<ChatRequest> chat_request = parseChatRequest(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!chat_request.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_message"}}));
      return;
    }
    if (chat_request->message.size() > kMaximumPromptBytes) {
      writer.send(http::jsonResponse(413, {{"error", "message_too_large"}}));
      return;
    }
    if (!chat_request->request_id.has_value()) {
      chat_request->request_id = internalRequestId();
    }
    if (chat_request->request_id.has_value() &&
        !progress_store.create(user_id.value(), chat_request->conversation_id, chat_request->request_id.value(),
                               generalProgressStages())) {
      writer.send(http::jsonResponse(409, {{"error", "chat_request_conflict"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), request = chat_request.value(), writer,
                                     &chat_service, &progress_store, &web_search,
                                     executor](db::MySqlConnection& connection) mutable {
      try {
        persistence::ChatConversationRepository conversations(connection);
        const std::optional<persistence::ChatConversation> conversation =
            conversations.findOwned(user_id, request.conversation_id);
        if (!conversation.has_value()) {
          failProgress(progress_store, user_id, request.request_id);
          writer.send(http::jsonResponse(404, {{"error", "conversation_not_found"}}));
          return;
        }
        if (conversation->mode != ConversationMode::kGeneral) {
          failProgress(progress_store, user_id, request.request_id);
          writer.send(http::jsonResponse(409, {{"error", "conversation_mode_mismatch"}}));
          return;
        }
        persistence::ChatMessageRepository messages(connection);
        restoreConversation(chat_service, messages, conversation.value());
        const std::string title = conversationTitle(request.message);
        const bool title_updated = conversations.updateTitleIfDefault(
            user_id, request.conversation_id, title, util::currentTimeMilliseconds());
        (void)title_updated;
        std::function<void()> prepare_task = [user_id, request = std::move(request), writer, &chat_service,
                                              &progress_store, &web_search]() mutable {
          try {
            const web_search::WebPreparation preparation = web_search.prepare(
                user_id, "general", std::to_string(request.conversation_id), request.message, "",
                request.web_search_enabled, request.web_search_consent_token,
                [user_id, request_id = request.request_id,
                 &progress_store](web_search::WebPreparationStage stage) {
                  if (!request_id.has_value()) {
                    return;
                  }
                  std::size_t stage_index = 0;
                  if (stage == web_search::WebPreparationStage::kPrivacy) {
                    stage_index = 1;
                  } else if (stage == web_search::WebPreparationStage::kSearch) {
                    stage_index = 2;
                  } else if (stage == web_search::WebPreparationStage::kFetch) {
                    stage_index = 3;
                  } else if (stage == web_search::WebPreparationStage::kEvidence) {
                    stage_index = 4;
                  }
                  if (stage_index > 0) {
                    const bool advanced = progress_store.advance(user_id, request_id.value(), stage_index);
                    (void)advanced;
                  }
                });
            if (preparation.status != web_search::WebPreparationStatus::kReady) {
              failProgress(progress_store, user_id, request.request_id);
              writer.send(webPreparationResponse(preparation));
              return;
            }

            const ChatSubmitStatus status = chat_service.submit(
                user_id, ConversationMode::kGeneral, request.conversation_id, std::move(request.message),
                preparation.system_context,
                [user_id, request_id = request.request_id, writer, &progress_store](ChatCompletion completion) {
                  if (completion.status == ChatCompletionStatus::kSuccess) {
                    if (request_id.has_value()) {
                      const bool completed = progress_store.complete(user_id, request_id.value());
                      (void)completed;
                    }
                  } else {
                    failProgress(progress_store, user_id, request_id);
                  }
                  writer.send(completionResponse(completion));
                },
                [user_id, request_id = request.request_id, &progress_store](ChatExecutionStage stage) {
                  if (!request_id.has_value()) {
                    return;
                  }
                  const std::size_t stage_index = stage == ChatExecutionStage::kModel ? 5 : 6;
                  const bool advanced = progress_store.advance(user_id, request_id.value(), stage_index);
                  (void)advanced;
                },
                [sources = preparation.sources](ChatCompletion completion) {
                  web_search::WebAnswerEnvelope envelope{completion.message.content, sources};
                  completion.message.content = web_search::serializeWebAnswer(envelope);
                  return completion;
                });
            if (status == ChatSubmitStatus::kBusy) {
              failProgress(progress_store, user_id, request.request_id);
              writer.send(http::jsonResponse(503, {{"error", "chat_busy"}}));
            } else if (status == ChatSubmitStatus::kInvalidPrompt) {
              failProgress(progress_store, user_id, request.request_id);
              writer.send(http::jsonResponse(400, {{"error", "invalid_message"}}));
            }
          } catch (const std::exception& error) {
            failProgress(progress_store, user_id, request.request_id);
            QAI_LOG(err, qaiservice::log::Module::kWebSearch) << "web_search_prepare_failed user_id=" << user_id
                                  << " conversation_id=" << request.conversation_id << " error=" << error.what();
            writer.send(http::jsonResponse(503, {{"error", "web_search_prepare_failed"}}));
          }
        };
        executor(std::move(prepare_task));
      } catch (const std::exception& error) {
        failProgress(progress_store, user_id, request.request_id);
        QAI_LOG(err, qaiservice::log::Module::kChat) << "conversation_submit user_id=" << user_id << " conversation_id=" << request.conversation_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "conversation_prepare_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      failProgress(progress_store, user_id.value(), chat_request->request_id);
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });
}

}  // namespace qaiservice::chat
