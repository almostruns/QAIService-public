#pragma once

#include "http/http_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace qaiservice::db {
class DatabaseWorker;
}

namespace qaiservice::http {
class Router;
}

namespace qaiservice::knowledge {

class FileStorage;

using DocumentSubmissionSink = std::function<bool(std::uint64_t, std::uint64_t)>;

[[nodiscard]] std::optional<std::string> decodeDocumentFilename(std::string_view encoded_filename);
[[nodiscard]] std::optional<std::string> validateDocumentUpload(const http::Request& request);
void registerKnowledgeRoutes(http::Router& router, db::DatabaseWorker& database_worker, FileStorage& storage,
                             DocumentSubmissionSink submission_sink);

}  // namespace qaiservice::knowledge
