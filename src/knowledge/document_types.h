#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace qaiservice::knowledge {

enum class DocumentStatus {
  kQueued,
  kProcessing,
  kReady,
  kFailed,
};

enum class LearningStatus {
  kUnreviewed,
  kLearning,
  kMastered,
  kNeedsReview,
};

struct StoredFile {
  std::string storage_key;
  std::string sha256_hex;
  std::uint64_t size_bytes;
};

struct NewDocument {
  std::uint64_t user_id;
  std::optional<std::uint64_t> folder_id;
  std::string original_name;
  std::string media_type;
  std::string storage_key;
  std::string sha256_hex;
  std::uint64_t size_bytes;
};

struct Document {
  std::uint64_t id;
  std::uint64_t user_id;
  std::optional<std::uint64_t> folder_id;
  std::string original_name;
  std::string media_type;
  std::string storage_key;
  std::string sha256_hex;
  std::uint64_t size_bytes;
  DocumentStatus status;
  std::string error_code;
  LearningStatus learning_status{LearningStatus::kUnreviewed};
};

struct NewDocumentChunk {
  std::uint32_t chunk_index;
  std::string content;
  std::optional<std::uint32_t> page_number{};
};

struct CreateDocumentResult {
  bool created;
  std::optional<Document> document;
};

}  // namespace qaiservice::knowledge
