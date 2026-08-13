#include "knowledge/knowledge_routes.h"

#include "db/database_worker.h"
#include "http/json_response.h"
#include "http/router.h"
#include "knowledge/document_repository.h"
#include "knowledge/file_storage.h"
#include "knowledge/knowledge_answer_service.h"
#include "knowledge/retriever.h"
#include "users/auth_middleware.h"

#include "logging/log.h"
#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace qaiservice::knowledge {
namespace {

std::optional<std::uint64_t> pathId(const http::Request& request, const std::string& name)
{
  const auto value = request.path_parameters.find(name);
  if (value == request.path_parameters.end()) {
    return std::nullopt;
  }
  std::uint64_t id = 0;
  const auto result = std::from_chars(value->second.data(), value->second.data() + value->second.size(), id);
  if (result.ec != std::errc{} || result.ptr != value->second.data() + value->second.size() || id == 0) {
    return std::nullopt;
  }
  return id;
}

std::string statusName(DocumentStatus status)
{
  switch (status) {
    case DocumentStatus::kQueued:
      return "queued";
    case DocumentStatus::kProcessing:
      return "processing";
    case DocumentStatus::kReady:
      return "ready";
    case DocumentStatus::kFailed:
      return "failed";
  }
  return "failed";
}

std::string learningStatusName(LearningStatus status)
{
  switch (status) {
    case LearningStatus::kUnreviewed:
      return "unreviewed";
    case LearningStatus::kLearning:
      return "learning";
    case LearningStatus::kMastered:
      return "mastered";
    case LearningStatus::kNeedsReview:
      return "needs_review";
  }
  return "unreviewed";
}

std::optional<LearningStatus> parseLearningStatus(const std::string& status)
{
  if (status == "unreviewed") {
    return LearningStatus::kUnreviewed;
  }
  if (status == "learning") {
    return LearningStatus::kLearning;
  }
  if (status == "mastered") {
    return LearningStatus::kMastered;
  }
  if (status == "needs_review") {
    return LearningStatus::kNeedsReview;
  }
  return std::nullopt;
}

nlohmann::json documentJson(const Document& document)
{
  return {{"id", document.id},
          {"filename", document.original_name},
          {"media_type", document.media_type},
          {"size_bytes", document.size_bytes},
          {"status", statusName(document.status)},
          {"learning_status", learningStatusName(document.learning_status)},
          {"error_code", document.error_code}};
}

bool safeFilename(std::string_view filename)
{
  if (filename.empty() || filename.size() > 255 || filename == "." || filename == "..") {
    return false;
  }
  for (const unsigned char value : filename) {
    if (value < 32 || value == 127 || value == '/' || value == '\\' || value == '"') {
      return false;
    }
  }
  return true;
}

bool supportedMediaType(const std::string& media_type)
{
  static const std::unordered_set<std::string> supported{
      "text/plain", "text/markdown", "application/pdf",
      "application/vnd.openxmlformats-officedocument.wordprocessingml.document"};
  return supported.find(media_type) != supported.end();
}

int hexValue(char value)
{
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

void sendDatabaseBusy(const http::ResponseWriter& writer)
{
  writer.send(http::jsonResponse(503, {{"error", "database_busy"}}));
}

}  // namespace

std::optional<std::string> decodeDocumentFilename(std::string_view encoded_filename)
{
  std::string decoded;
  decoded.reserve(encoded_filename.size());
  for (std::size_t index = 0; index < encoded_filename.size(); ++index) {
    if (encoded_filename[index] != '%') {
      decoded.push_back(encoded_filename[index]);
      continue;
    }
    if (index + 2 >= encoded_filename.size()) {
      return std::nullopt;
    }
    const int high = hexValue(encoded_filename[index + 1]);
    const int low = hexValue(encoded_filename[index + 2]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>(high * 16 + low));
    index += 2;
  }
  return decoded;
}

std::optional<std::string> validateDocumentUpload(const http::Request& request)
{
  const auto filename = request.headers.find("x-qai-filename");
  if (filename == request.headers.end()) {
    return "missing_filename";
  }
  const std::optional<std::string> decoded_filename = decodeDocumentFilename(filename->second);
  if (!decoded_filename.has_value() || !safeFilename(decoded_filename.value())) {
    return "invalid_filename";
  }
  const auto media_type = request.headers.find("content-type");
  if (media_type == request.headers.end() || !supportedMediaType(media_type->second)) {
    return "unsupported_media_type";
  }
  if (request.body.empty()) {
    return "empty_document";
  }
  if (request.body.size() > 8 * 1024 * 1024) {
    return "document_too_large";
  }
  return std::nullopt;
}

void registerKnowledgeRoutes(http::Router& router, db::DatabaseWorker& database_worker, FileStorage& storage,
                             DocumentSubmissionSink submission_sink)
{
  router.add("POST", "/api/documents",
             [&database_worker, &storage, submission_sink](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    const std::optional<std::string> validation_error = validateDocumentUpload(request);
    if (validation_error.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", validation_error.value()}}));
      return;
    }

    const std::string encoded_filename = request.headers.at("x-qai-filename");
    const std::string filename = decodeDocumentFilename(encoded_filename).value();
    const std::string media_type = request.headers.at("content-type");
    db::DatabaseWorker::Task task = [user_id = user_id.value(), filename, media_type, body = std::move(request.body),
                                     &storage, submission_sink, writer](db::MySqlConnection& connection) {
      try {
        const StoredFile stored = storage.store(user_id, filename, body);
        NewDocument input{user_id, std::nullopt, filename, media_type, stored.storage_key, stored.sha256_hex,
                          stored.size_bytes};
        DocumentRepository repository(connection);
        const CreateDocumentResult result = repository.create(input);
        if (!result.document.has_value()) {
          storage.remove(user_id, stored.storage_key);
          writer.send(http::jsonResponse(500, {{"error", "document_creation_failed"}}));
          return;
        }
        if (!result.created) {
          storage.remove(user_id, stored.storage_key);
          QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_upload_deduplicated user_id=" << user_id
                                                              << " document_id=" << result.document->id
                                                              << " size_bytes=" << stored.size_bytes;
          writer.send(http::jsonResponse(200, {{"duplicate", true}, {"document", documentJson(result.document.value())}}));
          return;
        }
        if (submission_sink && !submission_sink(user_id, result.document->id)) {
          repository.updateStatus(user_id, result.document->id, DocumentStatus::kFailed, "submission_unavailable");
          writer.send(http::jsonResponse(503, {{"error", "document_submission_unavailable"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_upload_queued user_id=" << user_id
                                                            << " document_id=" << result.document->id
                                                            << " size_bytes=" << stored.size_bytes;
        writer.send(http::jsonResponse(202, {{"duplicate", false}, {"document", documentJson(result.document.value())}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_upload user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(500, {{"error", "document_upload_failed"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("GET", "/api/documents", [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), writer](db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        nlohmann::json documents = nlohmann::json::array();
        for (const Document& document : repository.listOwned(user_id)) {
          documents.push_back(documentJson(document));
        }
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "documents_listed user_id=" << user_id
                                                            << " count=" << documents.size();
        writer.send(http::jsonResponse(200, {{"documents", std::move(documents)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_list user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("GET", "/api/documents/{document_id}",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> document_id = pathId(request, "document_id");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!document_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_document_id"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), document_id = document_id.value(), writer](
                                        db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        const std::optional<Document> document = repository.findOwned(user_id, document_id);
        if (!document.has_value()) {
          writer.send(http::jsonResponse(404, {{"error", "document_not_found"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_loaded user_id=" << user_id
                                                            << " document_id=" << document_id;
        writer.send(http::jsonResponse(200, {{"document", documentJson(document.value())}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_get user_id=" << user_id << " document_id=" << document_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("POST", "/api/documents/{document_id}/reindex",
             [&database_worker, submission_sink](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> document_id = pathId(request, "document_id");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!document_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_document_id"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), document_id = document_id.value(),
                                     submission_sink, writer](db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        if (!repository.findOwned(user_id, document_id).has_value()) {
          writer.send(http::jsonResponse(404, {{"error", "document_not_found"}}));
          return;
        }
        if (!submission_sink || !submission_sink(user_id, document_id)) {
          writer.send(http::jsonResponse(503, {{"error", "document_submission_unavailable"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_reindex_queued user_id=" << user_id
                                                            << " document_id=" << document_id;
        writer.send(http::jsonResponse(202, {{"status", "queued"}, {"document_id", document_id}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_reindex user_id=" << user_id << " document_id=" << document_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("GET", "/api/documents/{document_id}/content",
             [&database_worker, &storage](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> document_id = pathId(request, "document_id");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!document_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_document_id"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), document_id = document_id.value(), &storage, writer](
                                        db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        const std::optional<Document> document = repository.findOwned(user_id, document_id);
        if (!document.has_value()) {
          writer.send(http::jsonResponse(404, {{"error", "document_not_found"}}));
          return;
        }
        std::ifstream input(storage.pathFor(user_id, document->storage_key), std::ios::binary);
        if (!input) {
          writer.send(http::jsonResponse(404, {{"error", "document_content_missing"}}));
          return;
        }
        const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        http::Response response{200, document->media_type, content};
        response.headers["Cache-Control"] = "no-store";
        response.headers["Content-Disposition"] = "inline; filename=\"" + document->original_name + "\"";
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "document_content_served user_id=" << user_id
                                                            << " document_id=" << document_id
                                                            << " size_bytes=" << content.size();
        writer.send(std::move(response));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "document_content user_id=" << user_id << " document_id=" << document_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("POST", "/api/knowledge/ask",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    std::string query;
    try {
      const nlohmann::json body = nlohmann::json::parse(request.body);
      if (!body.is_object() || !body.contains("query") || !body["query"].is_string()) {
        writer.send(http::jsonResponse(400, {{"error", "invalid_query"}}));
        return;
      }
      query = body["query"].get<std::string>();
    } catch (const nlohmann::json::exception&) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_query"}}));
      return;
    }
    if (query.empty() || query.size() > 1000) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_query"}}));
      return;
    }

    db::DatabaseWorker::Task task = [user_id = user_id.value(), query = std::move(query), writer](
                                        db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        const std::vector<SearchCandidate> candidates = repository.listSearchCandidatesOwned(user_id);
        Retriever retriever;
        const std::vector<Evidence> evidence = retriever.search(query, candidates, 5);
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "knowledge_search user_id=" << user_id << " candidates=" << candidates.size()
                 << " evidence=" << evidence.size();
        KnowledgeAnswerService answer_service;
        nlohmann::json sources = nlohmann::json::array();
        for (const Evidence& item : evidence) {
          nlohmann::json source{{"document_id", item.document_id},
                                {"filename", item.filename},
                                {"chunk_index", item.chunk_index},
                                {"excerpt", item.excerpt},
                                {"score", item.score}};
          if (item.page_number.has_value()) {
            source["page"] = item.page_number.value();
          }
          sources.push_back(std::move(source));
        }
        writer.send(http::jsonResponse(200, {{"mode", "mock"},
                                       {"answer", answer_service.answer(evidence)},
                                       {"evidence", std::move(sources)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "knowledge_search user_id=" << user_id << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "knowledge_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });

  router.add("PATCH", "/api/knowledge/{item_id}/status",
             [&database_worker](http::Request request, http::ResponseWriter writer) {
    const std::optional<std::uint64_t> user_id = users::authenticatedUserId(request);
    const std::optional<std::uint64_t> document_id = pathId(request, "item_id");
    if (!user_id.has_value()) {
      writer.send(http::jsonResponse(401, {{"error", "authentication_required"}}));
      return;
    }
    if (!document_id.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_knowledge_item_id"}}));
      return;
    }
    std::optional<LearningStatus> status;
    try {
      const nlohmann::json body = nlohmann::json::parse(request.body);
      if (body.is_object() && body.contains("status") && body["status"].is_string()) {
        status = parseLearningStatus(body["status"].get<std::string>());
      }
    } catch (const nlohmann::json::exception&) {
    }
    if (!status.has_value()) {
      writer.send(http::jsonResponse(400, {{"error", "invalid_learning_status"}}));
      return;
    }
    db::DatabaseWorker::Task task = [user_id = user_id.value(), document_id = document_id.value(),
                                     status = status.value(), writer](db::MySqlConnection& connection) {
      try {
        DocumentRepository repository(connection);
        if (!repository.updateLearningStatus(user_id, document_id, status)) {
          writer.send(http::jsonResponse(404, {{"error", "knowledge_item_not_found"}}));
          return;
        }
        QAI_LOG(info, qaiservice::log::Module::kKnowledge) << "knowledge_status_updated user_id=" << user_id
                                                            << " document_id=" << document_id
                                                            << " status=" << learningStatusName(status);
        writer.send(http::jsonResponse(200, {{"id", document_id}, {"status", learningStatusName(status)}}));
      } catch (const std::exception& error) {
        QAI_LOG(err, qaiservice::log::Module::kKnowledge) << "knowledge_status user_id=" << user_id << " document_id=" << document_id
                  << " error=" << error.what();
        writer.send(http::jsonResponse(503, {{"error", "database_unavailable"}}));
      }
    };
    if (!database_worker.submit(std::move(task))) {
      sendDatabaseBusy(writer);
    }
  });
}

}  // namespace qaiservice::knowledge
