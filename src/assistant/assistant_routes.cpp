#include "assistant/assistant_routes.h"

#include "assistant/mock_intent_parser.h"
#include "assistant/private_assistant.h"
#include "assistant/proposal_store.h"
#include "chat/chat_progress_store.h"
#include "chat/chat_service.h"
#include "chat/conversation_title.h"
#include "db/database_worker.h"
#include "http/json_response.h"
#include "http/router.h"
#include "knowledge/document_repository.h"
#include "knowledge/rerank_client.h"
#include "knowledge/retriever.h"
#include "knowledge/token_client.h"
#include "life/life_record_repository.h"
#include "life/life_validation.h"
#include "persistence/chat_conversation_repository.h"
#include "persistence/chat_message_repository.h"
#include "users/auth_middleware.h"
#include "util/time.h"
#include "web_search/web_answer_envelope.h"
#include "web_search/web_search_coordinator.h"

#include "logging/log.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qaiservice::assistant {
namespace {

std::string internalRequestId()
{
  static std::atomic<std::uint64_t> next_id{1};
  return "private-internal-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
}

std::string todayLocalDate(std::int64_t now_ms)
{
  const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
  return buffer;
}

struct AssistantChatRequest {
  std::uint64_t conversation_id;
  std::string message;
  std::optional<std::string> request_id;
  bool web_search_enabled;
  std::optional<std::string> web_search_consent_token;
};

std::optional<AssistantChatRequest> parseAssistantChatRequest(const http::Request& request)
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
    AssistantChatRequest parsed{body["conversation_id"].get<std::uint64_t>(),
                                body["message"].get<std::string>(), std::move(request_id), web_search_enabled,
                                std::move(consent_token)};
    if (parsed.conversation_id == 0 || parsed.message.empty()) {
      return std::nullopt;
    }
    return parsed;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

std::vector<chat::ChatProgressStage> privateProgressStages()
{
  return {{"context", "读取私人上下文"}, {"retrieve", "检索相关资料"}, {"rerank", "语义重排"},
          {"evidence", "组织本地证据"}, {"decide", "判断是否需要联网"}, {"privacy", "检查查询隐私"},
          {"search", "搜索并筛选来源"}, {"fetch", "获取网页正文"}, {"web_evidence", "整理网页证据"},
          {"model", "基于证据思考"}, {"persist", "保存回答"},
          {"complete", "完成"}};
}

void advanceProgress(chat::ChatProgressStore& store, std::uint64_t user_id,
                     const std::optional<std::string>& request_id, std::size_t stage_index)
{
  if (request_id.has_value()) {
    const bool advanced = store.advance(user_id, request_id.value(), stage_index);
    (void)advanced;
  }
}

void failProgress(chat::ChatProgressStore& store, std::uint64_t user_id,
                  const std::optional<std::string>& request_id)
{
  if (request_id.has_value()) {
    const bool failed = store.fail(user_id, request_id.value());
    (void)failed;
  }
}

void restorePrivateConversation(chat::ChatService& service,
                                persistence::ChatMessageRepository& messages,
                                const persistence::ChatConversation& conversation)
{
  const std::vector<persistence::ChatMessageCreated> stored =
      messages.findByConversationOrdered(conversation.user_id, conversation.id);
  std::vector<chat::ChatMessage> history;
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

std::string persistenceName(chat::ChatPersistenceStatus status)
{
  switch (status) {
    case chat::ChatPersistenceStatus::kNotConfigured:
      return "not_configured";
    case chat::ChatPersistenceStatus::kQueued:
      return "queued";
    case chat::ChatPersistenceStatus::kBusy:
      return "busy";
    case chat::ChatPersistenceStatus::kUnavailable:
      return "unavailable";
  }
  return "unknown";
}

const char* rerankStatusName(knowledge::RerankStatus status)
{
  switch (status) {
    case knowledge::RerankStatus::kSuccess:
      return "success";
    case knowledge::RerankStatus::kDisabled:
      return "disabled";
    case knowledge::RerankStatus::kTimeout:
      return "timeout";
    case knowledge::RerankStatus::kUnavailable:
      return "unavailable";
    case knowledge::RerankStatus::kUpstreamError:
      return "upstream_error";
    case knowledge::RerankStatus::kInvalidResponse:
      return "invalid_response";
  }
  return "unknown";
}

const char* tokenStatusName(knowledge::TokenStatus status)
{
  switch (status) {
    case knowledge::TokenStatus::kSuccess:
      return "success";
    case knowledge::TokenStatus::kDisabled:
      return "disabled";
    case knowledge::TokenStatus::kTimeout:
      return "timeout";
    case knowledge::TokenStatus::kUnavailable:
      return "unavailable";
    case knowledge::TokenStatus::kUpstreamError:
      return "upstream_error";
    case knowledge::TokenStatus::kInvalidResponse:
      return "invalid_response";
  }
  return "unknown";
}

nlohmann::json evidenceJson(const std::vector<knowledge::Evidence>& evidence)
{
  nlohmann::json result = nlohmann::json::array();
  for (const knowledge::Evidence& item : evidence) {
    nlohmann::json entry{{"document_id", item.document_id},
                         {"filename", item.filename},
                         {"chunk_index", item.chunk_index},
                         {"excerpt", item.excerpt}};
    if (item.page_number.has_value()) {
      entry["page_number"] = item.page_number.value();
    }
    result.push_back(std::move(entry));
  }
  return result;
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

http::Response privateWebPreparationResponse(const web_search::WebPreparation& preparation)
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

http::Response privateCompletionError(chat::ChatCompletionStatus status)
{
  switch (status) {
    case chat::ChatCompletionStatus::kInvalidRequest:
      return http::jsonResponse(400, {{"error", "model_invalid_request"}});
    case chat::ChatCompletionStatus::kTimeout:
      return http::jsonResponse(504, {{"error", "model_timeout"}});
    case chat::ChatCompletionStatus::kUnavailable:
      return http::jsonResponse(503, {{"error", "model_unavailable"}});
    case chat::ChatCompletionStatus::kUpstreamError:
      return http::jsonResponse(502, {{"error", "model_upstream_error"}});
    case chat::ChatCompletionStatus::kInvalidResponse:
    case chat::ChatCompletionStatus::kSuccess:
      return http::jsonResponse(502, {{"error", "assistant_invalid_response"}});
  }
  return http::jsonResponse(500, {{"error", "internal_server_error"}});
}

void submitPrivateChat(std::uint64_t user_id, std::uint64_t conversation_id, std::string prompt,
                       const std::vector<knowledge::SearchCandidate>& all_candidates,
                       const std::vector<life::LifeRecord>& records, http::ResponseWriter writer,
                       ProposalStore& proposals, chat::ChatService& chat_service,
                       chat::ChatProgressStore& progress_store, knowledge::RerankClient& rerank_client,
                       knowledge::TokenClient& token_client, web_search::WebSearchCoordinator& web_search,
                       std::optional<std::string> request_id,
                       bool web_search_enabled,
                       std::optional<std::string> web_search_consent_token)
{
  knowledge::Retriever retriever;
  advanceProgress(progress_store, user_id, request_id, 1);
  const std::vector<knowledge::SearchCandidate> candidates =
      retriever.selectCandidates(prompt, all_candidates, 64);
  knowledge::RerankResult rerank{knowledge::RerankStatus::kUnavailable};
  advanceProgress(progress_store, user_id, request_id, 2);
  try {
    rerank = rerank_client.rerank(prompt, candidates);
  } catch (const std::exception& error) {
    QAI_LOG(err, qaiservice::log::Module::kAssistant) << "private_assistant_rerank user_id=" << user_id << " error=" << error.what();
  }
  std::vector<knowledge::Evidence> evidence;
  if (rerank.status == knowledge::RerankStatus::kSuccess) {
    evidence = retriever.fuse(prompt, candidates, rerank.scores, 5);
  } else {
    evidence = retriever.search(prompt, candidates, 5);
  }

  advanceProgress(progress_store, user_id, request_id, 3);
  knowledge::TokenFitResult token_fit{knowledge::TokenStatus::kDisabled};
  if (!evidence.empty()) {
    std::vector<std::string> excerpts;
    excerpts.reserve(evidence.size());
    for (const knowledge::Evidence& item : evidence) {
      excerpts.push_back(item.excerpt);
    }
    token_fit = token_client.fit(excerpts, 4000);
    if (token_fit.status == knowledge::TokenStatus::kSuccess) {
      evidence.resize(token_fit.texts.size());
      for (std::size_t index = 0; index < evidence.size(); ++index) {
        evidence[index].excerpt = std::move(token_fit.texts[index]);
      }
    }
  }

  const PrivateContext context = buildPrivateContext(evidence, records);
  QAI_LOG(info, qaiservice::log::Module::kAssistant) << "private_assistant_context user_id=" << user_id << " candidates=" << candidates.size()
           << " evidence=" << context.evidence.size() << " life_records=" << records.size()
           << " rerank_status=" << rerankStatusName(rerank.status)
           << " tokenizer_status=" << tokenStatusName(token_fit.status)
           << " evidence_tokens=" << token_fit.token_count;
  const web_search::WebPreparation web_preparation = web_search.prepare(
      user_id, "private", std::to_string(conversation_id), prompt, context.system_prompt,
      web_search_enabled, web_search_consent_token,
      [user_id, request_id, &progress_store](web_search::WebPreparationStage stage) {
        std::size_t stage_index = 4;
        if (stage == web_search::WebPreparationStage::kPrivacy) {
          stage_index = 5;
        } else if (stage == web_search::WebPreparationStage::kSearch) {
          stage_index = 6;
        } else if (stage == web_search::WebPreparationStage::kFetch) {
          stage_index = 7;
        } else if (stage == web_search::WebPreparationStage::kEvidence) {
          stage_index = 8;
        }
        advanceProgress(progress_store, user_id, request_id, stage_index);
      });
  if (web_preparation.status != web_search::WebPreparationStatus::kReady) {
    failProgress(progress_store, user_id, request_id);
    writer.send(privateWebPreparationResponse(web_preparation));
    return;
  }
  std::string combined_system_context = context.system_prompt;
  if (!web_preparation.system_context.empty()) {
    combined_system_context += "\n\n" + web_preparation.system_context;
  }
  const chat::ChatSubmitStatus status = chat_service.submit(
      user_id, chat::ConversationMode::kPrivate, conversation_id, std::move(prompt), combined_system_context,
      [user_id, request_id, evidence = context.evidence, web_sources = web_preparation.sources, writer, &proposals,
       &progress_store](chat::ChatCompletion completion) {
    if (completion.status != chat::ChatCompletionStatus::kSuccess) {
      failProgress(progress_store, user_id, request_id);
      writer.send(privateCompletionError(completion.status));
      return;
    }
    const std::optional<PrivateCompletion> parsed =
        parsePrivateCompletion(completion.message.content, util::currentTimeMilliseconds());
    if (!parsed.has_value()) {
      failProgress(progress_store, user_id, request_id);
      writer.send(privateCompletionError(chat::ChatCompletionStatus::kInvalidResponse));
      return;
    }
    nlohmann::json body{{"message", parsed->message},
                        {"persistence", persistenceName(completion.persistence)},
                        {"evidence", evidenceJson(evidence)},
                        {"web_sources", webSourcesJson(web_sources)}};
    if (parsed->kind == PrivateCompletionKind::kProposal) {
      const std::string token = proposals.create(user_id, parsed->command.value());
      QAI_LOG(info, qaiservice::log::Module::kAssistant) << "assistant_proposal_created user_id=" << user_id
                                                          << " domain=" << parsed->command->domain;
      body["requires_confirmation"] = true;
      body["proposal_token"] = token;
      body["preview"] = {{"summary", parsed->command->summary},
                         {"domain", parsed->command->domain},
                         {"data", parsed->command->payload}};
    }
    if (request_id.has_value()) {
      const bool completed = progress_store.complete(user_id, request_id.value());
      (void)completed;
    }
    QAI_LOG(info, qaiservice::log::Module::kAssistant) << "private_assistant_completed user_id=" << user_id
                                                        << " evidence_count=" << evidence.size()
                                                        << " web_source_count=" << web_sources.size()
                                                        << " persistence=" << static_cast<int>(completion.persistence);
    writer.send(http::jsonResponse(200, body));
  },
      [user_id, request_id, &progress_store](chat::ChatExecutionStage stage) {
    const std::size_t stage_index = stage == chat::ChatExecutionStage::kModel ? 9 : 10;
    advanceProgress(progress_store, user_id, request_id, stage_index);
  },
      [sources = web_preparation.sources](chat::ChatCompletion completion) {
    const auto parsed = nlohmann::json::parse(completion.message.content, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
      nlohmann::json enriched = parsed;
      enriched["web_sources"] = webSourcesJson(sources);
      completion.message.content = enriched.dump();
    }
    return completion;
  });
  if (status == chat::ChatSubmitStatus::kBusy) {
    failProgress(progress_store, user_id, request_id);
    writer.send(http::jsonResponse(503, {{"error", "chat_busy"}}));
  } else if (status == chat::ChatSubmitStatus::kInvalidPrompt) {
    failProgress(progress_store, user_id, request_id);
    writer.send(http::jsonResponse(400, {{"error", "invalid_message"}}));
  }
}

bool referencedProjectExists(life::LifeRecordRepository& repository, std::uint64_t user_id,
                             const ToolCommand& command)
{
  if (command.domain != "focus" && command.domain != "checkins") {
    return true;
  }
  const char* field = command.domain == "focus" ? "project_id" : "habit_id";
  const char* expected_type = command.domain == "focus" ? "focus" : "checkin";
  const std::uint64_t project_id = command.payload[field].get<std::uint64_t>();
  const std::optional<life::LifeRecord> project = repository.findOwned(user_id, project_id);
  if (!project.has_value() || project->domain != "habits" || project->payload.value("archived", false)) {
    return false;
  }
  return !project->payload.contains("project_type") || project->payload.value("project_type", "") == expected_type;
}

bool activeFocusExists(life::LifeRecordRepository& repository, std::uint64_t user_id,
                       const ToolCommand& command)
{
  if (command.domain != "focus") {
    return false;
  }
  for (const life::LifeRecord& record : repository.listOwned(user_id, "focus")) {
    if (record.payload.value("status", "active") == "active") {
      return true;
    }
  }
  return false;
}

}  // namespace

void registerAssistantRoutes(http::Router& router, db::DatabaseWorker& database_worker, ProposalStore& proposals,
                             chat::ChatService& chat_service, chat::ChatProgressStore& progress_store,
                             knowledge::RerankClient& rerank_client, knowledge::TokenClient& token_client,
                             web_search::WebSearchCoordinator& web_search, AssistantExecutor assistant_executor)
{
  router.add("POST", "/api/assistant/chat",
             [&database_worker, &proposals, &chat_service, &progress_store, &rerank_client, &token_client,
              &web_search, assistant_executor](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    std::optional<AssistantChatRequest> chat_request = parseAssistantChatRequest(request);
    if (!chat_request.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_message"}}));
      return;
    }
    if (chat_request->message.size() > 4096) {
      writer.send(http::jsonResponse(413, {{"error", "message_too_large"}}));
      return;
    }
    if (!chat_request->request_id.has_value()) {
      chat_request->request_id = internalRequestId();
    }
    if (chat_request->request_id.has_value() &&
        !progress_store.create(user_id.value(), chat_request->conversation_id, chat_request->request_id.value(),
                               privateProgressStages())) {
      writer.send(http::jsonResponse(409, {{"error", "chat_request_conflict"}}));
      return;
    }

    db::DatabaseWorker::Task task = [user_id = user_id.value(), request = chat_request.value(), writer, &proposals,
                                     &chat_service, &progress_store, &rerank_client, &token_client,
                                     &web_search, assistant_executor](db::MySqlConnection& connection) {
      try {
        persistence::ChatConversationRepository conversations(connection);
        const std::optional<persistence::ChatConversation> conversation =
            conversations.findOwned(user_id, request.conversation_id);
        if (!conversation.has_value()) {
          failProgress(progress_store, user_id, request.request_id);
          writer.send(http::jsonResponse(404, {{"error", "conversation_not_found"}}));
          return;
        }
        if (conversation->mode != chat::ConversationMode::kPrivate) {
          failProgress(progress_store, user_id, request.request_id);
          writer.send(http::jsonResponse(409, {{"error", "conversation_mode_mismatch"}}));
          return;
        }
        persistence::ChatMessageRepository messages(connection);
        restorePrivateConversation(chat_service, messages, conversation.value());
        const std::string title = chat::conversationTitle(request.message);
        const bool title_updated = conversations.updateTitleIfDefault(
            user_id, request.conversation_id, title, util::currentTimeMilliseconds());
        (void)title_updated;

        knowledge::DocumentRepository document_repository(connection);
        std::vector<knowledge::SearchCandidate> candidates =
            document_repository.listSearchCandidatesOwned(user_id);

        life::LifeRecordRepository life_repository(connection);
        std::vector<life::LifeRecord> records;
        for (const char* domain : {"habits", "focus", "checkins", "calendar", "tasks", "ledger", "weight",
                                   "sleep"}) {
          std::vector<life::LifeRecord> domain_records = life_repository.listOwned(user_id, domain);
          const std::size_t remaining = records.size() < 30 ? 30 - records.size() : 0;
          const std::size_t take = std::min(remaining, domain_records.size());
          records.insert(records.end(), domain_records.begin(), domain_records.begin() + take);
          if (records.size() == 30) {
            break;
          }
        }
        const std::size_t candidate_count = candidates.size();
        const std::size_t life_record_count = records.size();
        AssistantTask rerank_task = [user_id, conversation_id = request.conversation_id,
                                     prompt = request.message, candidates = std::move(candidates),
                                     records = std::move(records), request_id = request.request_id, writer,
                                     &proposals, &chat_service, &progress_store,
                                     &rerank_client, &token_client, &web_search,
                                     web_search_enabled = request.web_search_enabled,
                                     web_search_consent_token = request.web_search_consent_token]() mutable {
          try {
            submitPrivateChat(user_id, conversation_id, std::move(prompt), candidates, records, writer,
                              proposals, chat_service, progress_store, rerank_client, token_client,
                              web_search, request_id, web_search_enabled, web_search_consent_token);
          } catch (const std::exception& error) {
            failProgress(progress_store, user_id, request_id);
            QAI_LOG(err, qaiservice::log::Module::kAssistant) << "private_assistant_submit user_id=" << user_id << " conversation_id=" << conversation_id
                      << " error=" << error.what();
            writer.send(http::jsonResponse(503, {{"error", "private_context_unavailable"}}));
          }
        };
        QAI_LOG(info, qaiservice::log::Module::kAssistant) << "private_context_loaded user_id=" << user_id
                                                            << " conversation_id=" << request.conversation_id
                                                            << " candidate_count=" << candidate_count
                                                            << " life_record_count=" << life_record_count;
        assistant_executor(std::move(rerank_task));
      } catch (const std::exception& error) {
        failProgress(progress_store, user_id, request.request_id);
        QAI_LOG(err, qaiservice::log::Module::kAssistant) << "private_assistant_prepare user_id=" << user_id << " conversation_id="
                  << request.conversation_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "private_context_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      failProgress(progress_store, user_id.value(), chat_request->request_id);
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });

  router.add("POST", "/api/assistant/interpret",
             [&database_worker, &proposals](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    std::string text;
    try {
      const nlohmann::json body = nlohmann::json::parse(request.body);
      if (body.is_object() && body.contains("text") && body["text"].is_string()) {
        text = body["text"].get<std::string>();
      }
    } catch (const nlohmann::json::exception&) {
    }
    if (text.empty() || text.size() > 500) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_assistant_text"}}));
      return;
    }
    MockIntentParser parser;
    IntentResult intent = parser.parse(text, util::currentTimeMilliseconds());
    if (!intent.command.has_value()) {
      writer.send(http::jsonResponse(200, {{"mode", "mock"}, {"clarification", intent.clarification}}));
      return;
    }
    if (intent.command->domain == "checkins" && intent.command->payload.contains("habit_name") &&
        intent.command->payload["habit_name"].is_string()) {
      const std::string habit_name = intent.command->payload["habit_name"].get<std::string>();
      db::DatabaseWorker::Task task =
          [user_id = user_id.value(), habit_name, writer, &proposals](db::MySqlConnection& connection) {
        try {
          life::LifeRecordRepository repository(connection);
          const std::optional<std::string> requested_key =
              life::lifeRecordDedupeKey("habits", {{"name", habit_name}, {"project_type", "checkin"}});
          std::optional<life::LifeRecord> matching_project;
          for (const life::LifeRecord& project : repository.listOwned(user_id, "habits")) {
            if (project.payload.value("archived", false)) {
              continue;
            }
            if (life::lifeRecordDedupeKey("habits", project.payload) == requested_key) {
              matching_project = project;
              break;
            }
          }
          if (!matching_project.has_value()) {
            writer.send(http::jsonResponse(200, {{"mode", "mock"},
                                           {"clarification", "没有找到名为“" + habit_name +
                                                                  "”的打卡项目，请先创建打卡项目。"}}));
            return;
          }
          ToolCommand command{"checkins",
                              {{"habit_id", matching_project->id},
                               {"local_date", todayLocalDate(util::currentTimeMilliseconds())}},
                              "完成“" + habit_name + "”打卡"};
          const life::LifeValidationResult validation =
              life::validateLifeRecord(command.domain, command.payload);
          if (!validation.valid()) {
            writer.send(http::jsonResponse(400, {{"error", validation.error}}));
            return;
          }
          command.payload = validation.normalized;
          const std::string token = proposals.create(user_id, command);
          QAI_LOG(info, qaiservice::log::Module::kAssistant) << "assistant_proposal_created user_id=" << user_id
                                                              << " domain=" << command.domain;
          writer.send(http::jsonResponse(200, {{"mode", "mock"},
                                         {"requires_confirmation", true},
                                         {"proposal_token", token},
                                         {"preview", {{"summary", command.summary},
                                                      {"domain", command.domain},
                                                      {"data", command.payload}}}}));
        } catch (const std::exception& error) {
          QAI_LOG(err, qaiservice::log::Module::kAssistant) << "assistant_interpret user_id=" << user_id << " error=" << error.what();
          writer.send(http::jsonResponse(503, {{"error", "assistant_interpret_unavailable"}}));
        }
      };
      if (!database_worker.submit(std::move(task))) {
        writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
      }
      return;
    }
    const life::LifeValidationResult validation =
        life::validateLifeRecord(intent.command->domain, intent.command->payload);
    if (!validation.valid()) {
      writer.send(http::jsonResponse(400, {{"error", validation.error}}));
      return;
    }
    intent.command->payload = validation.normalized;
    const std::string token = proposals.create(user_id.value(), intent.command.value());
    QAI_LOG(info, qaiservice::log::Module::kAssistant) << "assistant_proposal_created user_id=" << user_id.value()
                                                        << " domain=" << intent.command->domain;
    writer.send(http::jsonResponse(200, {{"mode", "mock"},
                                   {"requires_confirmation", true},
                                   {"proposal_token", token},
                                   {"preview", {{"summary", intent.command->summary},
                                                {"domain", intent.command->domain},
                                                {"data", intent.command->payload}}}}));
  });

  router.add("POST", "/api/assistant/confirm",
             [&database_worker, &proposals](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    std::string token;
    try {
      const nlohmann::json body = nlohmann::json::parse(request.body);
      if (body.is_object() && body.contains("proposal_token") && body["proposal_token"].is_string()) {
        token = body["proposal_token"].get<std::string>();
      }
    } catch (const nlohmann::json::exception&) {
    }
    const std::optional<ToolCommand> command = proposals.consume(user_id.value(), token);
    if (!command.has_value()) {
      QAI_LOG(info, qaiservice::log::Module::kAssistant) << "assistant_confirmation_rejected user_id=" << user_id.value()
                                                          << " reason=invalid_or_expired";
      writer.send(http::jsonResponse(409, {{"error", "proposal_invalid_or_expired"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), command = command.value(), token, writer](
                                        db::MySqlConnection& connection) {
      try {
        life::LifeRecordRepository repository(connection);
        if (!referencedProjectExists(repository, user_id, command)) {
          writer.send(http::jsonResponse(409, {{"error", "activity_project_not_found"}}));
          return;
        }
        if (activeFocusExists(repository, user_id, command)) {
          writer.send(http::jsonResponse(409, {{"error", "focus_session_already_active"}}));
          return;
        }
        const std::optional<std::string> dedupe_key =
            life::lifeRecordDedupeKey(command.domain, command.payload);
        const life::CreateLifeRecordResult result = repository.create(
            user_id, command.domain, command.payload,
            life::lifeRecordOccurredAt(command.domain, command.payload, util::currentTimeMilliseconds()), dedupe_key);
        QAI_LOG(info, qaiservice::log::Module::kAssistant) << "assistant_write_confirmed user_id=" << user_id
                                                            << " domain=" << result.record.domain
                                                            << " record_id=" << result.record.id
                                                            << " created=" << result.created;
        writer.send(http::jsonResponse(201, {{"confirmed", true}, {"record_id", result.record.id},
                                       {"domain", result.record.domain}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kAssistant) << "assistant_confirm user_id=" << user_id << " domain=" << command.domain
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "assistant_write_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
    }
  });
}

}  // namespace qaiservice::assistant
