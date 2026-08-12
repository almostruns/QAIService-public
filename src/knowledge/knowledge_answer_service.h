#pragma once

#include "knowledge/retriever.h"

#include <string>
#include <vector>

namespace qaiservice::knowledge {

class KnowledgeAnswerService {
 public:
  [[nodiscard]] std::string answer(const std::vector<Evidence>& evidence) const;
};

}  // namespace qaiservice::knowledge
