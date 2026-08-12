#pragma once

#include "chat/chat_model_provider.h"
#include "web_search/query_privacy_guard.h"
#include "web_search/search_consent_store.h"
#include "web_search/web_evidence_service.h"
#include "web_search/web_search_router.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::web_search {

enum class WebPreparationStatus {
  kReady,
  kConsentRequired,
  kInvalidConsent,
  kPlannerUnavailable,
  kRequiredSearchUnavailable,
};

struct WebPreparation {
  WebPreparationStatus status{WebPreparationStatus::kPlannerUnavailable};
  std::string system_context;
  std::vector<WebSource> sources;
  std::string consent_token;
  std::vector<std::string> outbound_queries;
  std::vector<std::string> risk_types;
  WebEvidenceStatus evidence_status{WebEvidenceStatus::kNoEvidence};
};

enum class WebPreparationStage {
  kDecision,
  kPrivacy,
  kSearch,
  kFetch,
  kEvidence,
};

using WebPreparationProgress = std::function<void(WebPreparationStage)>;

class WebSearchCoordinator {
public:
  WebSearchCoordinator(std::unique_ptr<chat::ChatModelProvider> planning_model, WebEvidenceCollector& evidence,
                       SearchConsentStore& consents);

  [[nodiscard]] WebPreparation prepare(std::uint64_t user_id, const std::string& mode,
                                       const std::string& conversation_id, const std::string& message,
                                       const std::string& trusted_private_context,
                                       bool web_search_enabled,
                                       const std::optional<std::string>& consent_token,
                                       WebPreparationProgress progress = {});

private:
  [[nodiscard]] std::optional<WebSearchPlan> modelPlan(const std::string& message,
                                                      const std::string& trusted_private_context);

  std::unique_ptr<chat::ChatModelProvider> planning_model_;
  WebEvidenceCollector& evidence_;
  SearchConsentStore& consents_;
  WebSearchRouter router_;
  QueryPrivacyGuard privacy_guard_;
};

}  // namespace qaiservice::web_search
