#include "web_search/url_safety.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qaiservice::web_search {
namespace {

class SystemDnsResolver final : public DnsResolver {
public:
  [[nodiscard]] std::vector<std::string> resolve(const std::string& host) const override;
};

std::string lowercaseAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool invalidHostCharacter(unsigned char character)
{
  return std::isspace(character) != 0 || std::iscntrl(character) != 0 || character == '/' || character == '\\' ||
         character == '?' || character == '#';
}

bool publicIpv4(const in_addr& address)
{
  const std::uint32_t value = ntohl(address.s_addr);
  const auto in_block = [value](std::uint32_t network, std::uint32_t mask) {
    return (value & mask) == network;
  };
  return !in_block(0x00000000U, 0xff000000U) && !in_block(0x0a000000U, 0xff000000U) &&
         !in_block(0x64400000U, 0xffc00000U) && !in_block(0x7f000000U, 0xff000000U) &&
         !in_block(0xa9fe0000U, 0xffff0000U) && !in_block(0xac100000U, 0xfff00000U) &&
         !in_block(0xc0000000U, 0xffffff00U) && !in_block(0xc0000200U, 0xffffff00U) &&
         !in_block(0xc0a80000U, 0xffff0000U) && !in_block(0xc6120000U, 0xfffe0000U) &&
         !in_block(0xc6336400U, 0xffffff00U) && !in_block(0xcb007100U, 0xffffff00U) &&
         !in_block(0xe0000000U, 0xf0000000U) && !in_block(0xf0000000U, 0xf0000000U);
}

bool publicIpv6(const in6_addr& address)
{
  if (IN6_IS_ADDR_UNSPECIFIED(&address) || IN6_IS_ADDR_LOOPBACK(&address) || IN6_IS_ADDR_MULTICAST(&address) ||
      IN6_IS_ADDR_LINKLOCAL(&address)) {
    return false;
  }
  const unsigned char first = address.s6_addr[0];
  if ((first & 0xfeU) == 0xfcU) {
    return false;
  }
  if (address.s6_addr[0] == 0x20U && address.s6_addr[1] == 0x01U && address.s6_addr[2] == 0x0dU &&
      address.s6_addr[3] == 0xb8U) {
    return false;
  }
  if (IN6_IS_ADDR_V4MAPPED(&address)) {
    in_addr mapped{};
    std::memcpy(&mapped, &address.s6_addr[12], sizeof(mapped));
    return publicIpv4(mapped);
  }
  return (first & 0xe0U) == 0x20U;
}

bool publicIp(const std::string& text)
{
  in_addr ipv4{};
  if (inet_pton(AF_INET, text.c_str(), &ipv4) == 1) {
    return publicIpv4(ipv4);
  }
  in6_addr ipv6{};
  if (inet_pton(AF_INET6, text.c_str(), &ipv6) == 1) {
    return publicIpv6(ipv6);
  }
  return false;
}

bool literalIp(const std::string& text)
{
  in_addr ipv4{};
  in6_addr ipv6{};
  return inet_pton(AF_INET, text.c_str(), &ipv4) == 1 || inet_pton(AF_INET6, text.c_str(), &ipv6) == 1;
}

}  // namespace

std::vector<std::string> SystemDnsResolver::resolve(const std::string& host) const
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* raw_results = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &raw_results) != 0) {
    return {};
  }
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw_results, freeaddrinfo);
  std::set<std::string> unique;
  for (const addrinfo* entry = results.get(); entry != nullptr; entry = entry->ai_next) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    const void* address = nullptr;
    if (entry->ai_family == AF_INET) {
      address = &reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr;
    } else if (entry->ai_family == AF_INET6) {
      address = &reinterpret_cast<const sockaddr_in6*>(entry->ai_addr)->sin6_addr;
    }
    if (address != nullptr && inet_ntop(entry->ai_family, address, text.data(), text.size()) != nullptr) {
      unique.emplace(text.data());
    }
  }
  return {unique.begin(), unique.end()};
}

UrlSafety::UrlSafety() : resolver_(std::make_shared<SystemDnsResolver>())
{
}

UrlSafety::UrlSafety(std::shared_ptr<DnsResolver> resolver) : resolver_(std::move(resolver))
{
}

std::optional<SafeUrlTarget> UrlSafety::inspect(const std::string& url) const
{
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos || resolver_ == nullptr) {
    return std::nullopt;
  }
  const std::string scheme = lowercaseAscii(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") {
    return std::nullopt;
  }

  const std::size_t authority_start = scheme_end + 3;
  const std::size_t authority_end = url.find_first_of("/?#", authority_start);
  const std::string authority = url.substr(authority_start, authority_end - authority_start);
  if (authority.empty() || authority.find('@') != std::string::npos) {
    return std::nullopt;
  }

  std::string host;
  std::string port_text;
  if (authority.front() == '[') {
    const std::size_t bracket = authority.find(']');
    if (bracket == std::string::npos) {
      return std::nullopt;
    }
    host = authority.substr(1, bracket - 1);
    if (bracket + 1 < authority.size()) {
      if (authority[bracket + 1] != ':') {
        return std::nullopt;
      }
      port_text = authority.substr(bracket + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      host = authority;
    }
  }
  host = lowercaseAscii(host);
  if (host.empty() || std::any_of(host.begin(), host.end(), invalidHostCharacter)) {
    return std::nullopt;
  }

  std::uint16_t port = scheme == "https" ? 443 : 80;
  if (!port_text.empty()) {
    unsigned int parsed_port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || parsed_port > 65535) {
      return std::nullopt;
    }
    port = static_cast<std::uint16_t>(parsed_port);
  }
  if ((scheme == "http" && port != 80) || (scheme == "https" && port != 443)) {
    return std::nullopt;
  }

  std::vector<std::string> addresses;
  if (literalIp(host)) {
    addresses.push_back(host);
  } else {
    addresses = resolver_->resolve(host);
  }
  if (addresses.empty() || !std::all_of(addresses.begin(), addresses.end(), publicIp)) {
    return std::nullopt;
  }

  std::string path = authority_end == std::string::npos ? "/" : url.substr(authority_end);
  const std::size_t fragment = path.find('#');
  if (fragment != std::string::npos) {
    path.resize(fragment);
  }
  if (path.empty()) {
    path = "/";
  }
  return SafeUrlTarget{scheme, host, port, path, std::move(addresses)};
}

}  // namespace qaiservice::web_search
