#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace qaiservice::chat {

[[nodiscard]] std::string conversationTitle(std::string_view prompt, std::size_t maximum_characters = 32);

}  // namespace qaiservice::chat
