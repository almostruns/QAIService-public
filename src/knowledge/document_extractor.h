#pragma once

#include "knowledge/document_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace qaiservice::knowledge {

class DocumentExtractionError : public std::runtime_error {
 public:
  explicit DocumentExtractionError(const std::string& message);
};

struct ExtractedSection {
  std::optional<std::uint32_t> page_number;
  std::string text;
};

struct ExtractedDocument {
  std::vector<ExtractedSection> sections;
};

class DocumentExtractor {
 public:
  [[nodiscard]] ExtractedDocument extract(const std::string& media_type, const std::filesystem::path& path) const;
};

[[nodiscard]] std::vector<NewDocumentChunk> chunkDocument(const ExtractedDocument& document,
                                                          std::size_t target_bytes = 2000,
                                                          std::size_t hard_limit_bytes = 3000);

}  // namespace qaiservice::knowledge
