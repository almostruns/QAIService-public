#include "chat/conversation_title.h"

#include "util/utf8.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace qaiservice::chat {
namespace {

}  // namespace

std::string conversationTitle(std::string_view prompt, std::size_t maximum_characters)
{
  std::string title;
  bool pending_space = false;
  std::size_t characters = 0;
  for (std::size_t index = 0; index < prompt.size() && characters < maximum_characters;) {
    const unsigned char first = static_cast<unsigned char>(prompt[index]);
    if (first < 128 && std::isspace(first)) {
      pending_space = !title.empty();
      ++index;
      continue;
    }
    if (pending_space && characters < maximum_characters) {
      title.push_back(' ');
      ++characters;
      pending_space = false;
    }
    if (characters >= maximum_characters) {
      break;
    }
    const std::size_t bytes = util::utf8CodePointBytes(first);
    const std::size_t available = std::min(bytes, prompt.size() - index);
    title.append(prompt.substr(index, available));
    index += available;
    ++characters;
  }
  return title.empty() ? "新对话" : title;
}

}  // namespace qaiservice::chat
