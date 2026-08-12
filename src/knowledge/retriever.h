#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::knowledge {

struct SearchCandidate {
  std::uint64_t document_id;
  std::string filename;
  std::uint32_t chunk_index;
  std::optional<std::uint32_t> page_number;
  std::string content;
};

struct Evidence {
  std::uint64_t document_id;
  std::string filename;
  std::uint32_t chunk_index;
  std::optional<std::uint32_t> page_number;
  std::string excerpt;
  double score;
};

struct RerankScore {
  std::uint64_t document_id;
  std::uint32_t chunk_index;
  double score;
};

class Retriever {
 public:
  [[nodiscard]] std::vector<SearchCandidate> selectCandidates(
      const std::string& query, const std::vector<SearchCandidate>& candidates, std::size_t limit) const;
  [[nodiscard]] std::vector<Evidence> search(const std::string& query,
                                             const std::vector<SearchCandidate>& candidates,
                                             std::size_t limit) const;
  [[nodiscard]] std::vector<Evidence> fuse(const std::string& query,
                                           const std::vector<SearchCandidate>& candidates,
                                           const std::vector<RerankScore>& semantic_scores,
                                           std::size_t limit) const;
};

}  // namespace qaiservice::knowledge
