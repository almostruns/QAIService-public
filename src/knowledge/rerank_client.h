#pragma once

#include "knowledge/retriever.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace qaiservice::knowledge {

enum class RerankHttpStatus {
  kSuccess,
  kTimeout,
  kUnavailable,
};

struct RerankHttpRequest {
  std::string url;
  std::string body;
  std::chrono::milliseconds timeout{0};
};

struct RerankHttpResponse {
  RerankHttpStatus status{RerankHttpStatus::kUnavailable};
  long status_code{0};
  std::string body;
};

class RerankTransport {
 public:
  virtual ~RerankTransport() = default;

  [[nodiscard]] virtual RerankHttpResponse perform(const RerankHttpRequest& request) = 0;
};

struct RerankConfig {
  bool enabled{false};
  std::string endpoint;
  std::chrono::milliseconds timeout{3000};
};

enum class RerankStatus {
  kSuccess,
  kDisabled,
  kTimeout,
  kUnavailable,
  kUpstreamError,
  kInvalidResponse,
};

struct RerankResult {
  RerankStatus status{RerankStatus::kDisabled};
  std::string model;
  std::vector<RerankScore> scores;
};

[[nodiscard]] RerankConfig rerankConfigFromEnvironment();

class RerankClient {
 public:
  explicit RerankClient(RerankConfig config);
  RerankClient(RerankConfig config, std::shared_ptr<RerankTransport> transport);

  [[nodiscard]] RerankResult rerank(const std::string& query,
                                    const std::vector<SearchCandidate>& candidates) const;

 private:
  RerankConfig config_;
  std::shared_ptr<RerankTransport> transport_;
};

}  // namespace qaiservice::knowledge
