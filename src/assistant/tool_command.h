#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace qaiservice::assistant {

struct ToolCommand {
  std::string domain;
  nlohmann::json payload;
  std::string summary;
};

}  // namespace qaiservice::assistant
