#include "assistant/mock_intent_parser.h"

#include <cmath>
#include <cstdint>
#include <regex>
#include <string>

namespace qaiservice::assistant {

IntentResult MockIntentParser::parse(const std::string& text, std::int64_t now_ms) const
{
  std::smatch match;
  const std::regex weight_pattern(R"(^记录体重 ([0-9]{2,3})(?:\.([0-9]))? 公斤$)");
  if (std::regex_match(text, match, weight_pattern)) {
    const std::int64_t kilograms = std::stoll(match[1].str());
    const std::int64_t decimal = match[2].matched ? std::stoll(match[2].str()) : 0;
    const std::int64_t grams = kilograms * 1000 + decimal * 100;
    ToolCommand command{"weight", {{"grams", grams}, {"recorded_at_ms", now_ms}},
                        "记录体重 " + std::to_string(grams) + " 克"};
    return {std::move(command), ""};
  }

  const std::regex ledger_pattern(R"(^记录(支出|收入) ([0-9]+)(?:\.([0-9]{1,2}))? 元 (.+)$)");
  if (std::regex_match(text, match, ledger_pattern)) {
    const std::int64_t yuan = std::stoll(match[2].str());
    std::string fraction = match[3].matched ? match[3].str() : "";
    if (fraction.size() == 1) {
      fraction += "0";
    }
    const std::int64_t cents = fraction.empty() ? 0 : std::stoll(fraction);
    const std::int64_t amount_minor = yuan * 100 + cents;
    const std::string direction = match[1].str() == "支出" ? "expense" : "income";
    ToolCommand command{"ledger",
                        {{"direction", direction}, {"amount_minor", amount_minor}, {"currency", "CNY"},
                         {"category", match[4].str()}, {"occurred_at_ms", now_ms}},
                        "记录" + match[1].str() + " " + std::to_string(amount_minor) + " 分"};
    return {std::move(command), ""};
  }

  const std::regex focus_pattern(R"(^开始专注 ([0-9]{1,3}) 分钟$)");
  if (std::regex_match(text, match, focus_pattern)) {
    const std::int64_t minutes = std::stoll(match[1].str());
    ToolCommand command{"focus", {{"started_at_ms", now_ms}, {"planned_minutes", minutes}},
                        "开始专注 " + std::to_string(minutes) + " 分钟"};
    return {std::move(command), ""};
  }

  const std::regex habit_pattern(R"(^今天完成(.+)打卡$)");
  if (std::regex_match(text, match, habit_pattern)) {
    ToolCommand command{"checkins", {{"habit_name", match[1].str()}, {"local_date", "today"}},
                        "完成“" + match[1].str() + "”打卡"};
    return {std::move(command), ""};
  }
  return {std::nullopt, "我暂时只识别明确指令：记录体重、记录支出/收入、开始专注、今天完成某项打卡。"};
}

}  // namespace qaiservice::assistant
