#include "web_search/web_search_coordinator.h"

#include "logging/log.h"

#include <sodium.h>

#include <array>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace qaiservice::web_search {
namespace {

std::string messageDigest(const std::string& message)
{
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(message.data()), message.size());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
  return std::string(encoded.data());
}

std::string plannerSystemPrompt()
{
  return R"(你是联网搜索路由器。只输出一个 JSON 对象，字段必须严格为：
needs_web: boolean；required_for_answer: boolean；queries: string[]（0 到 3 条，每条不超过百度 72 字符限制）；reason: string；sensitive_fields_removed: string[]。
稳定知识无需联网；当前价格、天气、新闻、政策状态等时效事实必须联网。
如提供私人上下文，只能用它理解问题并生成去标识化的公共搜索词；删除姓名、手机号、邮箱、身份证、银行卡、精确门牌和任何可识别个人的信息。不要把私人上下文复制进 queries。)";
}

std::string plannerUserPrompt(const std::string& message, const std::string& private_context)
{
  std::string prompt = "用户问题：\n" + message;
  if (!private_context.empty()) {
    prompt += "\n\n受信任的私人上下文（只能用于理解和脱敏，不得原样输出）：\n" + private_context;
  }
  return prompt;
}

std::vector<std::string> queryRisks(const QueryPrivacyGuard& guard, const std::vector<std::string>& queries)
{
  std::set<std::string> unique;
  for (const std::string& query : queries) {
    const QueryPrivacyResult inspected = guard.inspect(query);
    unique.insert(inspected.risk_types.begin(), inspected.risk_types.end());
  }
  return {unique.begin(), unique.end()};
}

}  // namespace

WebSearchCoordinator::WebSearchCoordinator(std::unique_ptr<chat::ChatModelProvider> planning_model,
                                           WebEvidenceCollector& evidence, SearchConsentStore& consents)
    : planning_model_(std::move(planning_model)), evidence_(evidence), consents_(consents)
{
}

WebPreparation WebSearchCoordinator::prepare(std::uint64_t user_id, const std::string& mode,
                                             const std::string& conversation_id, const std::string& message,
                                             const std::string& trusted_private_context,
                                             bool web_search_enabled,
                                             const std::optional<std::string>& consent_token,
                                             WebPreparationProgress progress)
{
  if (!web_search_enabled) {
    QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_skipped user_id=" << user_id
                                                        << " mode=" << mode
                                                        << " conversation_id=" << conversation_id
                                                        << " reason=request_disabled";
    WebPreparation ready;
    ready.status = WebPreparationStatus::kReady;
    return ready;
  }
  if (progress) {
    progress(WebPreparationStage::kDecision);
  }
  std::optional<WebSearchPlan> plan;
  bool sensitive = false;
  if (consent_token.has_value()) {
    if (progress) {
      progress(WebPreparationStage::kPrivacy);
    }
    const std::string digest = messageDigest(message);
    const auto consent = consents_.consumeOwned(consent_token.value(), user_id, mode, conversation_id, digest);
    if (!consent.has_value()) {
      QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_consent_rejected user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id;
      return {WebPreparationStatus::kInvalidConsent};
    }
    WebSearchPlan approved_plan;
    approved_plan.needs_web = true;
    approved_plan.required_for_answer = consent->required_for_answer;
    approved_plan.queries = consent->queries;
    approved_plan.reason = "user_approved_sensitive_query";
    plan = std::move(approved_plan);
    sensitive = true;
    QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_consent_consumed user_id=" << user_id
                                                        << " mode=" << mode
                                                        << " conversation_id=" << conversation_id
                                                        << " query_count=" << plan->queries.size();
  } else {
    const std::optional<WebSearchPlan> deterministic = router_.deterministicRoute(message);
    if (deterministic.has_value() && !deterministic->needs_web) {
      QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_skipped user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id
                                                          << " reason=stable_knowledge";
      WebPreparation ready;
      ready.status = WebPreparationStatus::kReady;
      return ready;
    }
    plan = modelPlan(message, trusted_private_context);
    if (!plan.has_value() && deterministic.has_value() && deterministic->required_for_answer) {
      QAI_LOG(warn, qaiservice::log::Module::kWebSearch) << "web_search_planning_failed user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id
                                                          << " required=true";
      return {WebPreparationStatus::kPlannerUnavailable};
    }
    if (!plan.has_value()) {
      QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_skipped user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id
                                                          << " reason=planner_degraded";
      WebPreparation ready;
      ready.status = WebPreparationStatus::kReady;
      return ready;
    }
    if (deterministic.has_value() && deterministic->required_for_answer) {
      if (!plan->needs_web || plan->queries.empty()) {
        return {WebPreparationStatus::kPlannerUnavailable};
      }
      plan->required_for_answer = true;
      plan->reason = deterministic->reason;
    }
    if (!plan->needs_web) {
      QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_skipped user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id
                                                          << " reason=planner_not_needed";
      WebPreparation ready;
      ready.status = WebPreparationStatus::kReady;
      return ready;
    }

    if (progress) {
      progress(WebPreparationStage::kPrivacy);
    }
    const std::vector<std::string> risks = queryRisks(privacy_guard_, plan->queries);
    sensitive = !risks.empty();
    if (sensitive) {
      const std::string digest = messageDigest(message);
      WebPreparation confirmation;
      confirmation.status = WebPreparationStatus::kConsentRequired;
      confirmation.outbound_queries = plan->queries;
      confirmation.risk_types = risks;
      confirmation.consent_token = consents_.create(user_id, mode, conversation_id, digest,
                                                    plan->required_for_answer, plan->queries, risks);
      QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_consent_required user_id=" << user_id
                                                          << " mode=" << mode
                                                          << " conversation_id=" << conversation_id
                                                          << " query_count=" << plan->queries.size()
                                                          << " risk_count=" << risks.size();
      return confirmation;
    }
  }

  const WebEvidenceBundle bundle = evidence_.collect(
      plan.value(), sensitive, [progress](WebEvidenceStage stage) {
        if (!progress) {
          return;
        }
        if (stage == WebEvidenceStage::kSearch) {
          progress(WebPreparationStage::kSearch);
        } else if (stage == WebEvidenceStage::kFetch) {
          progress(WebPreparationStage::kFetch);
        } else {
          progress(WebPreparationStage::kEvidence);
        }
      });
  QAI_LOG(info, qaiservice::log::Module::kWebSearch) << "web_search_evidence_finished user_id=" << user_id
                                                      << " mode=" << mode
                                                      << " conversation_id=" << conversation_id
                                                      << " status=" << static_cast<int>(bundle.status)
                                                      << " source_count=" << bundle.sources.size()
                                                      << " sensitive=" << sensitive;
  if (bundle.status != WebEvidenceStatus::kSuccess && plan->required_for_answer) {
    WebPreparation unavailable;
    unavailable.status = WebPreparationStatus::kRequiredSearchUnavailable;
    unavailable.evidence_status = bundle.status;
    return unavailable;
  }

  WebPreparation ready;
  ready.status = WebPreparationStatus::kReady;
  ready.evidence_status = bundle.status;
  ready.sources = bundle.sources;
  if (bundle.status == WebEvidenceStatus::kSuccess) {
    ready.system_context = formatUntrustedEvidence(bundle);
  } else if (plan->needs_web) {
    ready.system_context = "本次未能完成联网验证。回答时必须明确说明未完成联网核实，不得把可能变化的信息表述为实时事实。";
  }
  return ready;
}

std::optional<WebSearchPlan> WebSearchCoordinator::modelPlan(const std::string& message,
                                                            const std::string& trusted_private_context)
{
  if (planning_model_ == nullptr) {
    return std::nullopt;
  }
  const std::vector<chat::ChatMessage> messages = {
      {chat::ChatRole::kSystem, plannerSystemPrompt()},
      {chat::ChatRole::kUser, plannerUserPrompt(message, trusted_private_context)}};
  const chat::ChatCompletion completion = planning_model_->complete(messages);
  if (completion.status != chat::ChatCompletionStatus::kSuccess) {
    return std::nullopt;
  }
  return parseModelPlan(completion.message.content);
}

}  // namespace qaiservice::web_search
