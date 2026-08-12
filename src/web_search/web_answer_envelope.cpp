#include "web_search/web_answer_envelope.h"

#include <nlohmann/json.hpp>

#include <string>

namespace qaiservice::web_search {

std::string serializeWebAnswer(const WebAnswerEnvelope& envelope)
{
  nlohmann::json sources = nlohmann::json::array();
  for (const WebSource& source : envelope.web_sources) {
    sources.push_back({{"id", source.id},
                       {"title", source.title},
                       {"url", source.url},
                       {"site", source.site},
                       {"published_at", source.published_at}});
  }
  return nlohmann::json{{"message", envelope.message}, {"web_sources", std::move(sources)}}.dump();
}

WebAnswerEnvelope parseWebAnswer(const std::string& content)
{
  const auto body = nlohmann::json::parse(content, nullptr, false);
  if (body.is_discarded() || !body.is_object() || !body.contains("message") || !body["message"].is_string() ||
      !body.contains("web_sources") || !body["web_sources"].is_array()) {
    return {content, {}};
  }

  WebAnswerEnvelope envelope;
  envelope.message = body["message"].get<std::string>();
  for (const auto& item : body["web_sources"]) {
    if (!item.is_object() || !item.contains("id") || !item["id"].is_number_unsigned() || !item.contains("title") ||
        !item["title"].is_string() || !item.contains("url") || !item["url"].is_string()) {
      continue;
    }
    WebSource source;
    source.id = item["id"].get<std::size_t>();
    source.title = item["title"].get<std::string>();
    source.url = item["url"].get<std::string>();
    if (item.contains("site") && item["site"].is_string()) {
      source.site = item["site"].get<std::string>();
    }
    if (item.contains("published_at") && item["published_at"].is_string()) {
      source.published_at = item["published_at"].get<std::string>();
    }
    envelope.web_sources.push_back(std::move(source));
  }
  return envelope;
}

}  // namespace qaiservice::web_search
