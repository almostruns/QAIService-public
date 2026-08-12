#include "knowledge/document_repository.h"

#include "db/mysql_connection.h"
#include "db/mysql_statement.h"

#include <mysql.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace qaiservice::knowledge {
namespace {

// 分块内容由 800 token 的分词器产生，单个 token 最多约 4 字节，但 UTF-8 边界对齐
// 可能让分块略超 3000 字节；读缓冲必须大于写入上限，避免 MYSQL_DATA_TRUNCATED。
constexpr std::size_t kMaximumChunkBytes = 64 * 1024;

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
  throw std::invalid_argument("unsupported document status");
}

DocumentStatus parseStatus(std::string_view status)
{
  if (status == "queued") {
    return DocumentStatus::kQueued;
  }
  if (status == "processing") {
    return DocumentStatus::kProcessing;
  }
  if (status == "ready") {
    return DocumentStatus::kReady;
  }
  if (status == "failed") {
    return DocumentStatus::kFailed;
  }
  throw db::DatabaseError("stored document has invalid status");
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
  throw std::invalid_argument("unsupported learning status");
}

LearningStatus parseLearningStatus(std::string_view status)
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
  throw db::DatabaseError("stored document has invalid learning status");
}

struct DocumentResultBuffers {
  std::uint64_t id{0};
  std::uint64_t user_id{0};
  std::uint64_t folder_id{0};
  std::array<char, 256> original_name{};
  std::array<char, 101> media_type{};
  std::array<char, 33> storage_key{};
  std::array<char, 65> sha256_hex{};
  std::uint64_t size_bytes{0};
  std::array<char, 11> status{};
  std::array<char, 65> error_code{};
  std::array<char, 13> learning_status{};
  std::array<unsigned long, 11> lengths{};
  std::array<my_bool, 11> nulls{};
  std::array<my_bool, 11> errors{};
  std::array<MYSQL_BIND, 11> bindings{};

  DocumentResultBuffers()
  {
    bindInteger(0, id, true);
    bindInteger(1, user_id, true);
    bindInteger(2, folder_id, true);
    bindText(3, original_name);
    bindText(4, media_type);
    bindText(5, storage_key);
    bindText(6, sha256_hex);
    bindInteger(7, size_bytes, true);
    bindText(8, status);
    bindText(9, error_code);
    bindText(10, learning_status);
  }

  [[nodiscard]] Document document() const
  {
    if (errors[0] || errors[1] || errors[3] || errors[4] || errors[5] || errors[6] || errors[7] || errors[8] ||
        errors[9] || nulls[0] || nulls[1] || nulls[3] || nulls[4] || nulls[5] || nulls[6] || nulls[7] ||
        nulls[8] || nulls[9] || errors[10] || nulls[10]) {
      throw db::DatabaseError("invalid document query result");
    }

    std::optional<std::uint64_t> optional_folder;
    if (!nulls[2]) {
      optional_folder = folder_id;
    }
    return {id,
            user_id,
            optional_folder,
            std::string(original_name.data(), lengths[3]),
            std::string(media_type.data(), lengths[4]),
            std::string(storage_key.data(), lengths[5]),
            std::string(sha256_hex.data(), lengths[6]),
            size_bytes,
            parseStatus(std::string_view(status.data(), lengths[8])),
            std::string(error_code.data(), lengths[9]),
            parseLearningStatus(std::string_view(learning_status.data(), lengths[10]))};
  }

 private:
  void bindInteger(std::size_t index, std::uint64_t& value, bool is_unsigned)
  {
    bindings[index].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[index].buffer = &value;
    bindings[index].is_unsigned = is_unsigned;
    bindings[index].is_null = &nulls[index];
    bindings[index].error = &errors[index];
  }

  template <std::size_t Size>
  void bindText(std::size_t index, std::array<char, Size>& value)
  {
    bindings[index].buffer_type = MYSQL_TYPE_STRING;
    bindings[index].buffer = value.data();
    bindings[index].buffer_length = static_cast<unsigned long>(value.size());
    bindings[index].length = &lengths[index];
    bindings[index].is_null = &nulls[index];
    bindings[index].error = &errors[index];
  }
};

constexpr char kDocumentColumns[] =
    "id, user_id, folder_id, original_name, media_type, storage_key, sha256_hex, size_bytes, status, error_code, "
    "learning_status";

}  // namespace

DocumentRepository::DocumentRepository(db::MySqlConnection& connection) : connection_(connection)
{
}

CreateDocumentResult DocumentRepository::create(const NewDocument& document)
{
  constexpr char sql[] =
      "INSERT INTO knowledge_documents "
      "(user_id, folder_id, original_name, media_type, storage_key, sha256_hex, size_bytes) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);

  std::uint64_t user_id = document.user_id;
  std::uint64_t folder_id = document.folder_id.value_or(0);
  my_bool folder_is_null = !document.folder_id.has_value();
  std::uint64_t size_bytes = document.size_bytes;
  std::array<unsigned long, 4> lengths{};
  std::array<MYSQL_BIND, 7> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &folder_id;
  parameters[1].is_unsigned = true;
  parameters[1].is_null = &folder_is_null;
  db::bindString(parameters[2], document.original_name, lengths[0]);
  db::bindString(parameters[3], document.media_type, lengths[1]);
  db::bindString(parameters[4], document.storage_key, lengths[2]);
  db::bindString(parameters[5], document.sha256_hex, lengths[3]);
  parameters[6].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[6].buffer = &size_bytes;
  parameters[6].is_unsigned = true;

  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot create knowledge document");
  }

  const bool created = mysql_stmt_affected_rows(statement.get()) == 1;
  const std::uint64_t document_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(statement.get()));
  return {created, findOwned(document.user_id, document_id)};
}

std::optional<Document> DocumentRepository::findOwned(std::uint64_t user_id, std::uint64_t document_id)
{
  const std::string sql = std::string("SELECT ") + kDocumentColumns +
                          " FROM knowledge_documents WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql.c_str());
  std::array<MYSQL_BIND, 2> parameters{};
  parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[0].buffer = &user_id;
  parameters[0].is_unsigned = true;
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &document_id;
  parameters[1].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot query owned document");
  }

  DocumentResultBuffers result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind owned document result");
  }
  const int fetched = mysql_stmt_fetch(statement.get());
  if (fetched == MYSQL_NO_DATA) {
    return std::nullopt;
  }
  if (fetched != 0) {
    throw db::DatabaseError("cannot fetch owned document");
  }
  return result.document();
}

std::vector<Document> DocumentRepository::listOwned(std::uint64_t user_id)
{
  const std::string sql = std::string("SELECT ") + kDocumentColumns +
                          " FROM knowledge_documents WHERE user_id = ? ORDER BY id DESC";
  db::PreparedStatement statement(connection_.nativeHandle(), sql.c_str());
  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_LONGLONG;
  parameter.buffer = &user_id;
  parameter.is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot list owned documents");
  }

  DocumentResultBuffers result;
  if (mysql_stmt_bind_result(statement.get(), result.bindings.data()) != 0) {
    throw db::DatabaseError("cannot bind document list result");
  }
  std::vector<Document> documents;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0) {
      throw db::DatabaseError("cannot fetch document list");
    }
    documents.push_back(result.document());
  }
  return documents;
}

void DocumentRepository::updateStatus(std::uint64_t user_id, std::uint64_t document_id, DocumentStatus status,
                                      std::string_view error_code_view)
{
  constexpr char sql[] = "UPDATE knowledge_documents SET status = ?, error_code = ? WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  const std::string status_value = statusName(status);
  const std::string error_code(error_code_view);
  std::array<unsigned long, 2> lengths{};
  std::array<MYSQL_BIND, 4> parameters{};
  db::bindString(parameters[0], status_value, lengths[0]);
  db::bindString(parameters[1], error_code, lengths[1]);
  parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[2].buffer = &user_id;
  parameters[2].is_unsigned = true;
  parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[3].buffer = &document_id;
  parameters[3].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot update document status");
  }
  if (mysql_stmt_affected_rows(statement.get()) == 0 && !findOwned(user_id, document_id).has_value()) {
    throw db::DatabaseError("cannot update missing owned document");
  }
}

bool DocumentRepository::updateLearningStatus(std::uint64_t user_id, std::uint64_t document_id, LearningStatus status)
{
  constexpr char sql[] = "UPDATE knowledge_documents SET learning_status = ? WHERE user_id = ? AND id = ?";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  const std::string status_value = learningStatusName(status);
  unsigned long status_length = 0;
  std::array<MYSQL_BIND, 3> parameters{};
  db::bindString(parameters[0], status_value, status_length);
  parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[1].buffer = &user_id;
  parameters[1].is_unsigned = true;
  parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
  parameters[2].buffer = &document_id;
  parameters[2].is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot update document learning status");
  }
  return mysql_stmt_affected_rows(statement.get()) > 0 || findOwned(user_id, document_id).has_value();
}

void DocumentRepository::replaceChunks(std::uint64_t user_id, std::uint64_t document_id,
                                       const std::vector<NewDocumentChunk>& chunks)
{
  if (!findOwned(user_id, document_id).has_value()) {
    throw db::DatabaseError("cannot replace chunks for missing owned document");
  }

  MYSQL* connection = connection_.nativeHandle();
  db::executeQuery(connection, "START TRANSACTION", "cannot start chunk transaction");
  try {
    constexpr char delete_sql[] = "DELETE FROM knowledge_chunks WHERE document_id = ?";
    db::PreparedStatement deletion(connection, delete_sql);
    MYSQL_BIND delete_parameter{};
    delete_parameter.buffer_type = MYSQL_TYPE_LONGLONG;
    delete_parameter.buffer = &document_id;
    delete_parameter.is_unsigned = true;
    if (mysql_stmt_bind_param(deletion.get(), &delete_parameter) != 0 || mysql_stmt_execute(deletion.get()) != 0) {
      throw db::DatabaseError("cannot clear document chunks");
    }

    constexpr char insert_sql[] =
        "INSERT INTO knowledge_chunks (document_id, chunk_index, page_number, content) VALUES (?, ?, ?, ?)";
    for (const NewDocumentChunk& chunk : chunks) {
      db::PreparedStatement insertion(connection, insert_sql);
      std::uint32_t chunk_index = chunk.chunk_index;
      std::uint32_t page_number = chunk.page_number.value_or(0);
      my_bool page_is_null = !chunk.page_number.has_value();
      unsigned long content_length = 0;
      std::array<MYSQL_BIND, 4> parameters{};
      parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
      parameters[0].buffer = &document_id;
      parameters[0].is_unsigned = true;
      parameters[1].buffer_type = MYSQL_TYPE_LONG;
      parameters[1].buffer = &chunk_index;
      parameters[1].is_unsigned = true;
      parameters[2].buffer_type = MYSQL_TYPE_LONG;
      parameters[2].buffer = &page_number;
      parameters[2].is_unsigned = true;
      parameters[2].is_null = &page_is_null;
      db::bindString(parameters[3], chunk.content, content_length);
      if (mysql_stmt_bind_param(insertion.get(), parameters.data()) != 0 || mysql_stmt_execute(insertion.get()) != 0) {
        throw db::DatabaseError("cannot insert document chunk");
      }
    }
    db::executeQuery(connection, "COMMIT", "cannot commit chunk transaction");
  } catch (...) {
    mysql_query(connection, "ROLLBACK");
    throw;
  }
}

std::vector<SearchCandidate> DocumentRepository::listSearchCandidatesOwned(std::uint64_t user_id)
{
  constexpr char sql[] =
      "SELECT documents.id, documents.original_name, chunks.chunk_index, chunks.page_number, chunks.content "
      "FROM knowledge_documents documents JOIN knowledge_chunks chunks ON chunks.document_id = documents.id "
      "WHERE documents.user_id = ? AND documents.status = 'ready' "
      "ORDER BY documents.id ASC, chunks.chunk_index ASC";
  db::PreparedStatement statement(connection_.nativeHandle(), sql);
  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_LONGLONG;
  parameter.buffer = &user_id;
  parameter.is_unsigned = true;
  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 || mysql_stmt_execute(statement.get()) != 0) {
    throw db::DatabaseError("cannot query searchable chunks");
  }

  std::uint64_t document_id = 0;
  std::array<char, 256> filename{};
  unsigned long filename_length = 0;
  std::uint32_t chunk_index = 0;
  std::uint32_t page_number = 0;
  my_bool page_is_null = false;
  std::vector<char> content(kMaximumChunkBytes);
  unsigned long content_length = 0;
  my_bool content_error = false;
  std::array<MYSQL_BIND, 5> results{};
  results[0].buffer_type = MYSQL_TYPE_LONGLONG;
  results[0].buffer = &document_id;
  results[0].is_unsigned = true;
  results[1].buffer_type = MYSQL_TYPE_STRING;
  results[1].buffer = filename.data();
  results[1].buffer_length = static_cast<unsigned long>(filename.size());
  results[1].length = &filename_length;
  results[2].buffer_type = MYSQL_TYPE_LONG;
  results[2].buffer = &chunk_index;
  results[2].is_unsigned = true;
  results[3].buffer_type = MYSQL_TYPE_LONG;
  results[3].buffer = &page_number;
  results[3].is_unsigned = true;
  results[3].is_null = &page_is_null;
  results[4].buffer_type = MYSQL_TYPE_STRING;
  results[4].buffer = content.data();
  results[4].buffer_length = static_cast<unsigned long>(content.size());
  results[4].length = &content_length;
  results[4].error = &content_error;
  if (mysql_stmt_bind_result(statement.get(), results.data()) != 0) {
    throw db::DatabaseError("cannot bind searchable chunk results");
  }

  std::vector<SearchCandidate> candidates;
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0 || content_error) {
      throw db::DatabaseError("cannot fetch searchable chunk");
    }
    std::optional<std::uint32_t> optional_page;
    if (!page_is_null) {
      optional_page = page_number;
    }
    candidates.push_back({document_id, std::string(filename.data(), filename_length), chunk_index, optional_page,
                          std::string(content.data(), content_length)});
  }
  return candidates;
}

}  // namespace qaiservice::knowledge
