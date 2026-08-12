#include "knowledge/retriever.h"

#include "util/utf8.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qaiservice::knowledge {
namespace {

constexpr double kFilenameWeight = 2.0;
constexpr double kMinimumSemanticScore = 0.01;
constexpr double kRelativeSemanticScore = 0.1;
constexpr double kRrfRankConstant = 60.0;

std::unordered_set<std::string> terms(std::string_view text)
{
  std::unordered_set<std::string> result;
  std::string ascii;
  std::vector<std::string> non_ascii;
  const auto flush_non_ascii = [&result, &non_ascii]() {
    if (non_ascii.size() == 1) {
      result.insert(non_ascii.front());
    }
    for (std::size_t index = 0; index + 1 < non_ascii.size(); ++index) {
      result.insert(non_ascii[index] + non_ascii[index + 1]);
    }
    non_ascii.clear();
  };
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char value = static_cast<unsigned char>(text[index]);
    if (value < 128) {
      if (std::isalnum(value)) {
        ascii.push_back(static_cast<char>(std::tolower(value)));
      } else if (!ascii.empty()) {
        result.insert(std::move(ascii));
        ascii.clear();
      }
      flush_non_ascii();
      ++index;
      continue;
    }
    if (!ascii.empty()) {
      result.insert(std::move(ascii));
      ascii.clear();
    }
    const std::size_t length = std::min(util::utf8CodePointBytes(value), text.size() - index);
    non_ascii.emplace_back(text.substr(index, length));
    index += length;
  }
  if (!ascii.empty()) {
    result.insert(std::move(ascii));
  }
  flush_non_ascii();
  return result;
}

std::size_t matchingTerms(const std::unordered_set<std::string>& query_terms,
                          const std::unordered_set<std::string>& candidate_terms)
{
  return static_cast<std::size_t>(std::count_if(query_terms.begin(), query_terms.end(), [&](const std::string& term) {
    return candidate_terms.find(term) != candidate_terms.end();
  }));
}

std::string candidateKey(std::uint64_t document_id, std::uint32_t chunk_index)
{
  return std::to_string(document_id) + ":" + std::to_string(chunk_index);
}

Evidence evidenceFromCandidate(const SearchCandidate& candidate, double score)
{
  return {candidate.document_id, candidate.filename, candidate.chunk_index, candidate.page_number,
          candidate.content, score};
}

}  // namespace

std::vector<SearchCandidate> Retriever::selectCandidates(
    const std::string& query, const std::vector<SearchCandidate>& candidates, std::size_t limit) const
{
  if (limit == 0) {
    return {};
  }

  const std::vector<Evidence> lexical = search(query, candidates, candidates.size());
  std::unordered_map<std::string, const SearchCandidate*> candidates_by_key;
  for (const SearchCandidate& candidate : candidates) {
    candidates_by_key[candidateKey(candidate.document_id, candidate.chunk_index)] = &candidate;
  }

  std::vector<SearchCandidate> selected;
  std::unordered_set<std::string> selected_keys;
  std::unordered_set<std::string> selected_content;
  const auto append_candidate = [&](const SearchCandidate& candidate) {
    const std::string key = candidateKey(candidate.document_id, candidate.chunk_index);
    if (selected.size() == limit || !selected_keys.insert(key).second ||
        !selected_content.insert(candidate.content).second) {
      return;
    }
    selected.push_back(candidate);
  };

  for (const Evidence& item : lexical) {
    const std::string key = candidateKey(item.document_id, item.chunk_index);
    const auto candidate = candidates_by_key.find(key);
    if (candidate != candidates_by_key.end()) {
      append_candidate(*candidate->second);
    }
  }
  for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
    append_candidate(*candidate);
  }
  return selected;
}

std::vector<Evidence> Retriever::search(const std::string& query, const std::vector<SearchCandidate>& candidates,
                                        std::size_t limit) const
{
  const std::unordered_set<std::string> query_terms = terms(query);
  if (query_terms.empty() || limit == 0) {
    return {};
  }

  std::vector<Evidence> evidence;
  std::unordered_set<std::string> seen_content;
  for (const SearchCandidate& candidate : candidates) {
    if (!seen_content.insert(candidate.content).second) {
      continue;
    }
    const std::unordered_set<std::string> content_terms = terms(candidate.content);
    const std::unordered_set<std::string> filename_terms = terms(candidate.filename);
    const std::size_t content_matches = matchingTerms(query_terms, content_terms);
    const std::size_t filename_matches = matchingTerms(query_terms, filename_terms);
    if (content_matches == 0 && filename_matches == 0) {
      continue;
    }
    const double query_size = static_cast<double>(query_terms.size());
    const double filename_size = static_cast<double>(std::max<std::size_t>(filename_terms.size(), 1));
    const double content_coverage = static_cast<double>(content_matches) / query_size;
    const double filename_coverage = static_cast<double>(filename_matches) / filename_size;
    const double score = content_coverage + kFilenameWeight * filename_coverage;
    evidence.push_back(evidenceFromCandidate(candidate, score));
  }
  std::sort(evidence.begin(), evidence.end(), [](const Evidence& left, const Evidence& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    if (left.document_id != right.document_id) {
      return left.document_id < right.document_id;
    }
    return left.chunk_index < right.chunk_index;
  });
  if (evidence.size() > limit) {
    evidence.resize(limit);
  }
  return evidence;
}

std::vector<Evidence> Retriever::fuse(const std::string& query, const std::vector<SearchCandidate>& candidates,
                                      const std::vector<RerankScore>& semantic_scores, std::size_t limit) const
{
  if (limit == 0) {
    return {};
  }
  const std::vector<Evidence> lexical = search(query, candidates, candidates.size());
  if (semantic_scores.empty()) {
    return {lexical.begin(), lexical.begin() + std::min(limit, lexical.size())};
  }

  std::unordered_map<std::string, const SearchCandidate*> candidates_by_key;
  for (const SearchCandidate& candidate : candidates) {
    candidates_by_key[candidateKey(candidate.document_id, candidate.chunk_index)] = &candidate;
  }

  std::unordered_map<std::string, double> fused_scores;
  for (std::size_t index = 0; index < lexical.size(); ++index) {
    const std::string key = candidateKey(lexical[index].document_id, lexical[index].chunk_index);
    fused_scores[key] += 1.0 / (kRrfRankConstant + static_cast<double>(index + 1));
  }

  std::vector<RerankScore> ordered_semantic = semantic_scores;
  std::sort(ordered_semantic.begin(), ordered_semantic.end(), [](const RerankScore& left, const RerankScore& right) {
    return left.score > right.score;
  });
  const double relative_threshold = ordered_semantic.front().score * kRelativeSemanticScore;
  const double semantic_threshold = std::max(kMinimumSemanticScore, relative_threshold);
  for (std::size_t index = 0; index < ordered_semantic.size(); ++index) {
    const RerankScore& item = ordered_semantic[index];
    if (item.score < semantic_threshold) {
      break;
    }
    const std::string key = candidateKey(item.document_id, item.chunk_index);
    if (candidates_by_key.find(key) != candidates_by_key.end()) {
      fused_scores[key] += 1.0 / (kRrfRankConstant + static_cast<double>(index + 1));
    }
  }

  std::vector<Evidence> fused;
  fused.reserve(fused_scores.size());
  for (const auto& [key, score] : fused_scores) {
    const auto candidate = candidates_by_key.find(key);
    if (candidate != candidates_by_key.end()) {
      fused.push_back(evidenceFromCandidate(*candidate->second, score));
    }
  }
  std::sort(fused.begin(), fused.end(), [](const Evidence& left, const Evidence& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    if (left.document_id != right.document_id) {
      return left.document_id < right.document_id;
    }
    return left.chunk_index < right.chunk_index;
  });
  if (fused.size() > limit) {
    fused.resize(limit);
  }
  return fused;
}

}  // namespace qaiservice::knowledge
