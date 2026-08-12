#pragma once

#include "http/http_types.h"

#include <nlohmann/json.hpp>

namespace qaiservice::http {

inline Response jsonResponse(int status, const nlohmann::json& body)
{
  Response response{status, "application/json", body.dump()};
  response.headers["Cache-Control"] = "no-store";
  return response;
}

}  // namespace qaiservice::http
