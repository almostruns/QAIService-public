#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace qaiservice::web_search {

struct ExtractedWebPage {
  std::string title;
  std::string text;
};

class WebPageExtractor {
public:
  explicit WebPageExtractor(std::size_t maximum_text_bytes);

  [[nodiscard]] std::optional<ExtractedWebPage> extract(const std::string& body,
                                                        const std::string& content_type) const;

private:
  std::size_t maximum_text_bytes_;
};

}  // namespace qaiservice::web_search
