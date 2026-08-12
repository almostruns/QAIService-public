#include "users/password_hasher.h"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <string>

namespace qaiservice::users {

PasswordHasher::PasswordHasher()
{
  if (sodium_init() < 0) {
    throw std::runtime_error("cannot initialize password hashing");
  }
}

std::string PasswordHasher::hash(std::string_view password) const
{
  std::array<char, crypto_pwhash_STRBYTES> output{};
  const int status = crypto_pwhash_str_alg(output.data(), password.data(), password.size(),
                                           crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                           crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                           crypto_pwhash_ALG_ARGON2ID13);
  if (status != 0) {
    throw std::runtime_error("cannot hash password");
  }
  return output.data();
}

bool PasswordHasher::verify(std::string_view hash_value, std::string_view password) const
{
  const std::string stored_hash(hash_value);
  return crypto_pwhash_str_verify(stored_hash.c_str(), password.data(), password.size()) == 0;
}

}  // namespace qaiservice::users
