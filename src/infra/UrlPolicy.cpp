#include "UrlPolicy.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <regex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct parsed_url {
  std::string scheme;
  std::string host;
  std::string path;
  bool has_query = false;
};

bool parse_url(const std::string& url, parsed_url& out) {
  static const std::regex re(R"(^([A-Za-z][A-Za-z0-9+.-]*):\/\/(\[[^\]]+\]|[^\/:?#]+)([^?#]*)?(\?.*)?$)");
  std::smatch m;
  if (!std::regex_search(url, m, re)) return false;
  out.scheme = to_lower(m[1].str());
  out.host = to_lower(m[2].str());
  out.path = m[3].matched ? m[3].str() : "";
  out.has_query = m[4].matched;
  if (!out.host.empty() && out.host.front() == '[' && out.host.back() == ']') {
    out.host = out.host.substr(1, out.host.size() - 2);
  }
  if (!out.host.empty() && out.host.back() == '.') out.host.pop_back();
  return !out.scheme.empty() && !out.host.empty();
}

bool domain_matches(const std::string& host, const std::string& domain) {
  std::string d = to_lower(domain);
  return host == d || ends_with(host, "." + d);
}

bool in_list(const std::string& host, const std::vector<std::string>& domains) {
  for (const auto& domain : domains) {
    if (domain_matches(host, domain)) return true;
  }
  return false;
}

bool is_ipv4_private_or_loopback(const std::string& host) {
  std::istringstream ss(host);
  std::string part;
  int parts[4]{};
  for (int i = 0; i < 4; ++i) {
    if (!std::getline(ss, part, '.')) return false;
    if (part.empty() || part.size() > 3) return false;
    if (!std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) {
      return false;
    }
    int value = std::stoi(part);
    if (value < 0 || value > 255) return false;
    parts[i] = value;
  }
  if (std::getline(ss, part, '.')) return false;

  if (parts[0] == 127) return true;
  if (parts[0] == 10) return true;
  if (parts[0] == 192 && parts[1] == 168) return true;
  if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) return true;
  if (parts[0] == 169 && parts[1] == 254) return true;
  if (parts[0] == 0) return true;
  return false;
}

bool is_blocked_ipv4_addr(const in_addr& addr) {
  const auto value = ntohl(addr.s_addr);
  const unsigned int a = (value >> 24) & 0xff;
  const unsigned int b = (value >> 16) & 0xff;

  if (a == 0 || a == 10 || a == 127) return true;
  if (a == 169 && b == 254) return true;
  if (a == 172 && b >= 16 && b <= 31) return true;
  if (a == 192 && b == 168) return true;
  if (a == 100 && b >= 64 && b <= 127) return true;
  if (a == 198 && (b == 18 || b == 19)) return true;
  if (a >= 224) return true;
  return false;
}

bool is_blocked_ipv6_addr(const in6_addr& addr) {
  const unsigned char* b = addr.s6_addr;
  bool all_zero = true;
  for (int i = 0; i < 16; ++i) {
    if (b[i] != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) return true;

  bool loopback = true;
  for (int i = 0; i < 15; ++i) {
    if (b[i] != 0) {
      loopback = false;
      break;
    }
  }
  if (loopback && b[15] == 1) return true;

  if ((b[0] & 0xfe) == 0xfc) return true;
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;
  if (b[0] == 0xff) return true;
  return false;
}

bool is_ip_literal_blocked(const std::string& host, bool& is_literal) {
  in_addr ipv4{};
  if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
    is_literal = true;
    return is_blocked_ipv4_addr(ipv4);
  }
  in6_addr ipv6{};
  if (inet_pton(AF_INET6, host.c_str(), &ipv6) == 1) {
    is_literal = true;
    return is_blocked_ipv6_addr(ipv6);
  }
  is_literal = false;
  return false;
}

bool is_private_or_local_host(const std::string& host) {
  if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1") return true;
  if (ends_with(host, ".localhost") || ends_with(host, ".local")) return true;
  if (is_ipv4_private_or_loopback(host)) return true;
  if (host.rfind("fc", 0) == 0 || host.rfind("fd", 0) == 0 || host.rfind("fe80", 0) == 0) {
    return true;
  }
  return false;
}

bool resolved_to_blocked_address(const std::string& host, std::string& reason) {
  bool is_literal = false;
  if (is_ip_literal_blocked(host, is_literal)) {
    reason = "private or special IP URL is blocked";
    return true;
  }
  if (is_literal) return false;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (rc != 0) {
    reason = "DNS resolve failed";
    return true;
  }

  bool has_address = false;
  bool blocked = false;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    has_address = true;
    if (it->ai_family == AF_INET) {
      auto* in = reinterpret_cast<sockaddr_in*>(it->ai_addr);
      if (is_blocked_ipv4_addr(in->sin_addr)) {
        blocked = true;
        break;
      }
    } else if (it->ai_family == AF_INET6) {
      auto* in6 = reinterpret_cast<sockaddr_in6*>(it->ai_addr);
      if (is_blocked_ipv6_addr(in6->sin6_addr)) {
        blocked = true;
        break;
      }
    }
  }
  freeaddrinfo(result);

  if (!has_address) {
    reason = "DNS resolve returned no addresses";
    return true;
  }
  if (blocked) reason = "domain resolves to private or special IP";
  return blocked;
}

}

bool is_allowed_url(const std::string& url, const security_config& cfg, std::string& reason) {
  parsed_url parsed;
  if (!parse_url(url, parsed)) {
    reason = "invalid URL";
    return false;
  }

  if (parsed.scheme != "http" && parsed.scheme != "https") {
    reason = "URL scheme is blocked";
    return false;
  }

  if (in_list(parsed.host, cfg.blocked_domains)) {
    reason = "domain is blocked";
    return false;
  }

  if (!cfg.allow_private_networks && is_private_or_local_host(parsed.host)) {
    reason = "private or localhost URL is blocked";
    return false;
  }

  if (!cfg.allow_private_networks && resolved_to_blocked_address(parsed.host, reason)) {
    return false;
  }

  if (to_lower(cfg.mode) == "strict" && !in_list(parsed.host, cfg.allowed_domains)) {
    reason = "domain is not in allowed_domains";
    return false;
  }

  reason.clear();
  return true;
}

std::string sanitize_url_for_log(const std::string& url) {
  parsed_url parsed;
  if (!parse_url(url, parsed)) return "<invalid-url>";
  std::string out = parsed.scheme + "://" + parsed.host + parsed.path;
  if (parsed.has_query) out += "?redacted=true";
  return out;
}
