#pragma once

#include "assistant/tool_command.h"

#include <cstdint>
#include <optional>
#include <string>

namespace qaiservice::assistant {

struct IntentResult {
  std::optional<ToolCommand> command;
  std::string clarification;
};

class MockIntentParser {
 public:
  [[nodiscard]] IntentResult parse(const std::string& text, std::int64_t now_ms) const;
};

}  // namespace qaiservice::assistant
