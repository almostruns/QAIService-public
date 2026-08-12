#include "assistant/private_assistant.h"

#include "life/life_validation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace qaiservice::assistant {
namespace {

constexpr std::size_t kMaximumCompletionBytes = 16 * 1024;
constexpr std::size_t kMaximumEvidence = 5;
constexpr std::size_t kMaximumRecords = 30;

bool nonEmptyString(const nlohmann::json& value, const char* field, std::size_t maximum)
{
  return value.contains(field) && value[field].is_string() && !value[field].get_ref<const std::string&>().empty() &&
         value[field].get_ref<const std::string&>().size() <= maximum;
}

std::optional<ToolCommand> parseCommand(const nlohmann::json& root, std::int64_t now_ms)
{
  if (!nonEmptyString(root, "tool", 64) || !root.contains("arguments") || !root["arguments"].is_object()) {
    return std::nullopt;
  }

  const std::string tool = root["tool"].get<std::string>();
  const nlohmann::json& arguments = root["arguments"];
  ToolCommand command;
  if (tool == "create_project") {
    if (!nonEmptyString(arguments, "name", 100) || !nonEmptyString(arguments, "project_type", 16) ||
        !nonEmptyString(arguments, "color", 7)) {
      return std::nullopt;
    }
    command.domain = "habits";
    command.payload = {{"name", arguments["name"]},
                       {"project_type", arguments["project_type"]},
                       {"color", arguments["color"]},
                       {"note", arguments.value("note", "")},
                       {"archived", false}};
    command.summary = "创建活动项目“" + arguments["name"].get<std::string>() + "”";
  } else if (tool == "start_focus") {
    if (!arguments.contains("project_id") || !arguments["project_id"].is_number_unsigned() ||
        !arguments.contains("planned_minutes") || !arguments["planned_minutes"].is_number_integer() ||
        !nonEmptyString(arguments, "timer_mode", 16)) {
      return std::nullopt;
    }
    command.domain = "focus";
    command.payload = {{"project_id", arguments["project_id"]},
                       {"started_at_ms", now_ms},
                       {"planned_minutes", arguments["planned_minutes"]},
                       {"timer_mode", arguments["timer_mode"]},
                       {"status", "active"}};
    command.summary = "开始专注计时";
  } else if (tool == "checkin") {
    if (!arguments.contains("project_id") || !arguments["project_id"].is_number_unsigned() ||
        !nonEmptyString(arguments, "local_date", 10)) {
      return std::nullopt;
    }
    command.domain = "checkins";
    command.payload = {{"habit_id", arguments["project_id"]}, {"local_date", arguments["local_date"]}};
    command.summary = "记录项目打卡";
  } else if (tool == "record_transaction") {
    if (!nonEmptyString(arguments, "direction", 7) || !arguments.contains("amount_minor") ||
        !arguments["amount_minor"].is_number_integer() || !nonEmptyString(arguments, "currency", 3) ||
        !nonEmptyString(arguments, "category", 100)) {
      return std::nullopt;
    }
    command.domain = "ledger";
    command.payload = {{"direction", arguments["direction"]},
                       {"amount_minor", arguments["amount_minor"]},
                       {"currency", arguments["currency"]},
                       {"category", arguments["category"]},
                       {"occurred_at_ms", now_ms}};
    const std::string action =
        arguments["direction"].get<std::string>() == "income" ? "收入" : "支出";
    command.summary = "记录" + action + " " +
                      std::to_string(arguments["amount_minor"].get<std::int64_t>()) + " 分（" +
                      arguments["category"].get<std::string>() + "）";
  } else if (tool == "record_weight") {
    if (!arguments.contains("grams") || !arguments["grams"].is_number_integer()) {
      return std::nullopt;
    }
    command.domain = "weight";
    command.payload = {{"grams", arguments["grams"]}, {"recorded_at_ms", now_ms}};
    command.summary =
        "记录体重 " + std::to_string(arguments["grams"].get<std::int64_t>()) + " 克";
  } else {
    return std::nullopt;
  }

  const life::LifeValidationResult validation = life::validateLifeRecord(command.domain, command.payload);
  if (!validation.valid()) {
    return std::nullopt;
  }
  command.payload = validation.normalized;
  return command;
}

}  // namespace

std::optional<PrivateCompletion> parsePrivateCompletion(const std::string& content, std::int64_t now_ms)
{
  if (content.empty() || content.size() > kMaximumCompletionBytes) {
    return std::nullopt;
  }

  try {
    const nlohmann::json root = nlohmann::json::parse(content);
    if (!root.is_object() || !nonEmptyString(root, "type", 32) || !nonEmptyString(root, "message", 2000)) {
      return std::nullopt;
    }
    const std::string type = root["type"].get<std::string>();
    const std::string message = root["message"].get<std::string>();
    if (type == "answer") {
      return PrivateCompletion{PrivateCompletionKind::kAnswer, message, std::nullopt};
    }
    if (type != "proposal") {
      return std::nullopt;
    }
    std::optional<ToolCommand> command = parseCommand(root, now_ms);
    if (!command.has_value()) {
      return std::nullopt;
    }
    return PrivateCompletion{PrivateCompletionKind::kProposal, message, std::move(command)};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

PrivateContext buildPrivateContext(const std::vector<knowledge::Evidence>& evidence,
                                   const std::vector<life::LifeRecord>& records)
{
  PrivateContext context;
  const std::size_t evidence_count = std::min(evidence.size(), kMaximumEvidence);
  context.evidence.assign(evidence.begin(), evidence.begin() + evidence_count);

  std::ostringstream prompt;
  prompt << "PRIVATE_ASSISTANT_JSON\n"
         << "你是用户的私人智能管家，可以使用通用知识和推理能力。涉及用户个人情况时，优先结合本段提供的私人资料和本次对话；资料不足以完成判断时，追问完成判断所必需的信息。不要把模型已有知识冒充为实时联网结果。\n"
         << "只输出单个 JSON 对象，不要 Markdown。普通答复格式："
         << R"({"type":"answer","message":"答复"})"
         << "。需要写入数据时只提出方案，不得声称已经执行；可用工具：\n"
         << R"(create_project {"name":"名称","project_type":"focus或checkin",)"
         << R"("color":"#RRGGBB","note":"可选备注"})" << '\n'
         << R"(start_focus {"project_id":正整数,"timer_mode":"countdown或stopwatch",)"
         << R"("planned_minutes":倒计时1到240或正计时0})" << '\n'
         << R"(checkin {"project_id":正整数,"local_date":"YYYY-MM-DD"})" << '\n'
         << R"(record_transaction {"direction":"expense或income","amount_minor":正整数分,)"
         << R"("currency":"三位大写币种","category":"分类"})" << '\n'
         << R"(record_weight {"grams":整数克})" << '\n'
         << "record_transaction 与 record_weight 的发生时间按当前时间，不需要提供。\n"
         << "写入方案格式："
         << R"({"type":"proposal","message":"说明","tool":"工具名","arguments":{}})"
         << "。不要输出其他工具或系统命令。\n\n本地知识证据：\n";

  for (const knowledge::Evidence& item : context.evidence) {
    prompt << "- document_id=" << item.document_id << ", filename=" << item.filename;
    if (item.page_number.has_value()) {
      prompt << ", page=" << item.page_number.value();
    }
    prompt << ", excerpt=" << item.excerpt << '\n';
  }

  prompt << "\n最近的本地生活记录：\n";
  const std::size_t record_count = std::min(records.size(), kMaximumRecords);
  for (std::size_t index = 0; index < record_count; ++index) {
    const life::LifeRecord& record = records[index];
    prompt << "- id=" << record.id << ", domain=" << record.domain
           << ", occurred_at_ms=" << record.occurred_at_ms << ", data=" << record.payload.dump() << '\n';
  }
  context.system_prompt = prompt.str();
  return context;
}

}  // namespace qaiservice::assistant
