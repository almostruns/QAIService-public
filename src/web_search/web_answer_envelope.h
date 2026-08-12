#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace qaiservice::web_search {

struct WebSource {
  std::size_t id{0};
  std::string title;
  std::string url;
  std::string site;
  std::string published_at;
};

struct WebAnswerEnvelope {
  std::string message;
  std::vector<WebSource> web_sources;
};

[[nodiscard]] std::string serializeWebAnswer(const WebAnswerEnvelope& envelope);
[[nodiscard]] WebAnswerEnvelope parseWebAnswer(const std::string& content);

}  // namespace qaiservice::web_search
