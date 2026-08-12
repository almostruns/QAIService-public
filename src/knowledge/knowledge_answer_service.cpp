#include "knowledge/knowledge_answer_service.h"

namespace qaiservice::knowledge {

std::string KnowledgeAnswerService::answer(const std::vector<Evidence>& evidence) const
{
  if (evidence.empty()) {
    return "当前知识库中没有找到足够相关的本地证据。";
  }
  return "根据本地资料，最相关的内容是：" + evidence.front().excerpt;
}

}  // namespace qaiservice::knowledge
