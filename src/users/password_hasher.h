#pragma once

#include <string>
#include <string_view>

namespace qaiservice::users {

class PasswordHasher {
 public:
  PasswordHasher();

  [[nodiscard]] std::string hash(std::string_view password) const;
  [[nodiscard]] bool verify(std::string_view hash, std::string_view password) const;
};

}  // namespace qaiservice::users
