#pragma once

#include "knowledge/document_types.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace qaiservice::knowledge {

class FileStorage {
 public:
  explicit FileStorage(std::filesystem::path root);

  [[nodiscard]] StoredFile store(std::uint64_t user_id, std::string_view original_name, std::string_view bytes) const;
  [[nodiscard]] std::filesystem::path pathFor(std::uint64_t user_id, std::string_view storage_key) const;
  void remove(std::uint64_t user_id, std::string_view storage_key) const;

 private:
  [[nodiscard]] std::filesystem::path userDirectory(std::uint64_t user_id) const;

  std::filesystem::path root_;
};

}  // namespace qaiservice::knowledge
