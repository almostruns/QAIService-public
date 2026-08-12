#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qaiservice::web_search {

class DnsResolver {
public:
  virtual ~DnsResolver() = default;

  [[nodiscard]] virtual std::vector<std::string> resolve(const std::string& host) const = 0;
};

struct SafeUrlTarget {
  std::string scheme;
  std::string host;
  std::uint16_t port{0};
  std::string path;
  std::vector<std::string> validated_ips;
};

class UrlSafety {
public:
  UrlSafety();
  explicit UrlSafety(std::shared_ptr<DnsResolver> resolver);

  [[nodiscard]] std::optional<SafeUrlTarget> inspect(const std::string& url) const;

private:
  std::shared_ptr<DnsResolver> resolver_;
};

}  // namespace qaiservice::web_search
