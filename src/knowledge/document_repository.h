#pragma once

#include "knowledge/document_types.h"
#include "knowledge/retriever.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace qaiservice::db {
class MySqlConnection;
}

namespace qaiservice::knowledge {

class DocumentRepository {
 public:
  explicit DocumentRepository(db::MySqlConnection& connection);

  [[nodiscard]] CreateDocumentResult create(const NewDocument& document);
  [[nodiscard]] std::optional<Document> findOwned(std::uint64_t user_id, std::uint64_t document_id);
  [[nodiscard]] std::vector<Document> listOwned(std::uint64_t user_id);
  void updateStatus(std::uint64_t user_id, std::uint64_t document_id, DocumentStatus status,
                    std::string_view error_code);
  [[nodiscard]] bool updateLearningStatus(std::uint64_t user_id, std::uint64_t document_id, LearningStatus status);
  void replaceChunks(std::uint64_t user_id, std::uint64_t document_id,
                     const std::vector<NewDocumentChunk>& chunks);
  [[nodiscard]] std::vector<SearchCandidate> listSearchCandidatesOwned(std::uint64_t user_id);

 private:
  db::MySqlConnection& connection_;
};

}  // namespace qaiservice::knowledge
