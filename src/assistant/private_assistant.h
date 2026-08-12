#pragma once

#include "assistant/tool_command.h"
#include "knowledge/retriever.h"
#include "life/life_record_repository.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::assistant {

enum class PrivateCompletionKind {
  kAnswer,
  kProposal,
};

struct PrivateCompletion {
  PrivateCompletionKind kind;
  std::string message;
  std::optional<ToolCommand> command;
};

struct PrivateContext {
  std::string system_prompt;
  std::vector<knowledge::Evidence> evidence;
};

[[nodiscard]] std::optional<PrivateCompletion> parsePrivateCompletion(const std::string& content,
                                                                      std::int64_t now_ms);
[[nodiscard]] PrivateContext buildPrivateContext(const std::vector<knowledge::Evidence>& evidence,
                                                 const std::vector<life::LifeRecord>& records);

}  // namespace qaiservice::assistant
