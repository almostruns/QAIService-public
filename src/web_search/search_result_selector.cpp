#include "web_search/search_result_selector.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qaiservice::web_search {
namespace {

struct ScoredResult {
  SearchResult result;
  std::string host;
  int score{0};
  std::size_t original_index{0};
};

std::string lowercaseAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string normalizeUrl(const std::string& url)
{
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return url;
  }
  const std::size_t authority_start = scheme_end + 3;
  const std::size_t path_start = url.find('/', authority_start);
  const std::size_t authority_end = path_start == std::string::npos ? url.size() : path_start;
  std::string normalized = lowercaseAscii(url.substr(0, authority_end));
  if (path_start != std::string::npos) {
    normalized += url.substr(path_start);
  }
  const std::size_t fragment = normalized.find('#');
  if (fragment != std::string::npos) {
    normalized.resize(fragment);
  }
  while (normalized.size() > authority_end + 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

std::string hostFromUrl(const std::string& url)
{
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return {};
  }
  const std::size_t start = scheme_end + 3;
  const std::size_t end = url.find_first_of("/:?#", start);
  return lowercaseAscii(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

int relevanceScore(const std::string& query, const SearchResult& result)
{
  if (query.empty()) {
    return 0;
  }
  int score = 0;
  if (result.title.find(query) != std::string::npos) {
    score += 100;
  }
  if (result.snippet.find(query) != std::string::npos) {
    score += 40;
  }
  const std::string searchable = lowercaseAscii(result.title + " " + result.snippet);
  std::set<std::string> seen_tokens;
  for (std::size_t index = 0; index < query.size();) {
    const unsigned char first = static_cast<unsigned char>(query[index]);
    std::size_t token_bytes = 1;
    if ((first & 0xe0U) == 0xc0U) {
      token_bytes = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
      token_bytes = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
      token_bytes = 4;
    }
    if (index + token_bytes > query.size()) {
      break;
    }
    std::string token = query.substr(index, token_bytes);
    index += token_bytes;
    if (token_bytes == 1) {
      const unsigned char character = static_cast<unsigned char>(token.front());
      if (std::isalnum(character) == 0) {
        continue;
      }
      token.front() = static_cast<char>(std::tolower(character));
    }
    if (!seen_tokens.insert(token).second) {
      continue;
    }
    if (searchable.find(token) != std::string::npos) {
      score += token_bytes == 1 ? 1 : 18;
    }
  }
  return score;
}

int authorityScore(const SearchResult& result, const std::string& host)
{
  const bool government = host.size() >= 6 && host.compare(host.size() - 6, 6, "gov.cn") == 0;
  const bool education = host.size() >= 6 && host.compare(host.size() - 6, 6, "edu.cn") == 0;
  const bool marked_official = result.site.find("官方") != std::string::npos || result.title.find("官方") != std::string::npos;
  return government || education || marked_official ? 80 : 0;
}

int recencyScore(const std::string& published_at)
{
  if (published_at.size() < 4 || !std::all_of(published_at.begin(), published_at.begin() + 4, ::isdigit)) {
    return 0;
  }
  const int year = std::stoi(published_at.substr(0, 4));
  return std::clamp(year - 2020, 0, 10) * 3;
}

}  // namespace

std::vector<SearchResult> SearchResultSelector::select(const std::string& query,
                                                       const std::vector<SearchResult>& candidates,
                                                       std::size_t maximum_results) const
{
  std::set<std::string> seen_urls;
  std::vector<ScoredResult> scored;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    SearchResult result = candidates[index];
    result.url = normalizeUrl(result.url);
    if (result.url.empty() || !seen_urls.insert(result.url).second) {
      continue;
    }
    const std::string host = hostFromUrl(result.url);
    const int score = relevanceScore(query, result) + authorityScore(result, host) + recencyScore(result.published_at);
    scored.push_back({std::move(result), host, score, index});
  }

  std::stable_sort(scored.begin(), scored.end(), [](const ScoredResult& left, const ScoredResult& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    return left.original_index < right.original_index;
  });

  std::map<std::string, std::size_t> host_counts;
  std::vector<SearchResult> selected;
  for (ScoredResult& candidate : scored) {
    if (selected.size() >= maximum_results) {
      break;
    }
    if (!candidate.host.empty() && host_counts[candidate.host] >= 2) {
      continue;
    }
    ++host_counts[candidate.host];
    selected.push_back(std::move(candidate.result));
  }
  return selected;
}

}  // namespace qaiservice::web_search
