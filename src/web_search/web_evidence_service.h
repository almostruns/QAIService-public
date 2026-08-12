#pragma once

#include "web_search/safe_page_fetcher.h"
#include "web_search/search_provider.h"
#include "web_search/search_result_selector.h"
#include "web_search/web_answer_envelope.h"
#include "web_search/web_cache.h"
#include "web_search/web_page_extractor.h"
#include "web_search/web_search_types.h"

#include <cstddef>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace qaiservice::web_search {

enum class WebEvidenceStatus {
  kSuccess,
  kNoEvidence,
  kNotConfigured,
  kTimeout,
  kRateLimited,
  kQuotaExhausted,
  kUnauthorized,
  kUpstreamError,
  kInvalidResponse,
};

struct WebEvidence {
  std::size_t id{0};
  std::string title;
  std::string url;
  std::string text;
};

struct WebEvidenceBundle {
  WebEvidenceStatus status{WebEvidenceStatus::kNoEvidence};
  bool required_for_answer{false};
  std::vector<WebEvidence> evidence;
  std::vector<WebSource> sources;
};

struct WebEvidenceConfig {
  std::size_t maximum_pages{6};
  std::size_t maximum_evidence_bytes{12000};
  std::chrono::milliseconds maximum_duration{12000};
};

enum class WebEvidenceStage {
  kSearch,
  kFetch,
  kEvidence,
};

using WebEvidenceProgress = std::function<void(WebEvidenceStage)>;

class WebEvidenceCollector {
public:
  virtual ~WebEvidenceCollector() = default;

  [[nodiscard]] virtual WebEvidenceBundle collect(const WebSearchPlan& plan, bool sensitive_query,
                                                  WebEvidenceProgress progress = {}) = 0;
};

class WebEvidenceService final : public WebEvidenceCollector {
public:
  WebEvidenceService(SearchProvider& provider, const SearchResultSelector& selector, const SafePageFetcher& fetcher,
                     const WebPageExtractor& extractor, SearchResponseCache& search_cache, PageBodyCache& page_cache,
                     WebEvidenceConfig config);

  [[nodiscard]] WebEvidenceBundle collect(const WebSearchPlan& plan, bool sensitive_query,
                                          WebEvidenceProgress progress = {}) override;

private:
  SearchProvider& provider_;
  const SearchResultSelector& selector_;
  const SafePageFetcher& fetcher_;
  const WebPageExtractor& extractor_;
  SearchResponseCache& search_cache_;
  PageBodyCache& page_cache_;
  WebEvidenceConfig config_;
};

[[nodiscard]] std::string formatUntrustedEvidence(const WebEvidenceBundle& bundle);

}  // namespace qaiservice::web_search
