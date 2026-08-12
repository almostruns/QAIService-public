#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace qaiservice::knowledge {

enum class TokenHttpStatus {
  kSuccess,
  kTimeout,
  kUnavailable,
};

struct TokenHttpRequest {
  std::string url;
  std::string body;
  std::chrono::milliseconds timeout{0};
};

struct TokenHttpResponse {
  TokenHttpStatus status{TokenHttpStatus::kUnavailable};
  long status_code{0};
  std::string body;
};

class TokenTransport {
 public:
  virtual ~TokenTransport() = default;

  [[nodiscard]] virtual TokenHttpResponse perform(const TokenHttpRequest& request) = 0;
};

struct TokenConfig {
  bool enabled{false};
  std::string base_url;
  std::chrono::milliseconds timeout{5000};
};

enum class TokenStatus {
  kSuccess,
  kDisabled,
  kTimeout,
  kUnavailable,
  kUpstreamError,
  kInvalidResponse,
};

struct TokenSplitResult {
  TokenStatus status{TokenStatus::kDisabled};
  std::string encoding;
  std::vector<std::string> chunks;
  std::vector<std::size_t> token_counts;
};

struct TokenFitResult {
  TokenStatus status{TokenStatus::kDisabled};
  std::string encoding;
  std::vector<std::string> texts;
  std::size_t token_count{0};
};

[[nodiscard]] TokenConfig tokenConfigFromEnvironment();

class TokenClient {
 public:
  explicit TokenClient(TokenConfig config);
  TokenClient(TokenConfig config, std::shared_ptr<TokenTransport> transport);

  [[nodiscard]] TokenSplitResult split(const std::string& text, std::size_t chunk_tokens = 800,
                                       std::size_t overlap_tokens = 400) const;
  [[nodiscard]] TokenFitResult fit(const std::vector<std::string>& texts,
                                   std::size_t maximum_tokens = 4000) const;

 private:
  TokenConfig config_;
  std::shared_ptr<TokenTransport> transport_;
};

}  // namespace qaiservice::knowledge
