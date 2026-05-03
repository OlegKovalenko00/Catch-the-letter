#include "MailClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct curl_global_guard {
  curl_global_guard() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~curl_global_guard() { curl_global_cleanup(); }
};

static curl_global_guard curl_guard;

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::uint64_t parse_uint64_or_zero(const std::string& text) {
  try {
    size_t pos = 0;
    std::uint64_t value = std::stoull(text, &pos);
    return pos == text.size() ? value : 0;
  } catch (...) {
    return 0;
  }
}

std::string get_header(const std::vector<std::string>& headers, const std::string& name) {
  std::string needle = to_lower(name);
  for (const auto& h : headers) {
    auto pos = h.find(':');
    if (pos == std::string::npos) continue;
    std::string key = to_lower(trim(h.substr(0, pos)));
    if (key == needle) return trim(h.substr(pos + 1));
  }
  return "";
}

std::string base64_decode(const std::string& input) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int bits = -8;
  for (unsigned char c : input) {
    if (std::isspace(c)) continue;
    if (c == '=') break;
    auto pos = alphabet.find(static_cast<char>(c));
    if (pos == std::string::npos) continue;
    val = (val << 6) + static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

std::string quoted_printable_decode(const std::string& input, bool header_mode) {
  std::string out;
  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (header_mode && c == '_') {
      out.push_back(' ');
      continue;
    }
    if (c == '=' && i + 2 < input.size()) {
      if (input[i + 1] == '\r' || input[i + 1] == '\n') continue;
      int hi = hex_value(input[i + 1]);
      int lo = hex_value(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

std::string decode_mime_header(const std::string& value) {
  static const std::regex encoded_word(R"(=\?([^?]+)\?([bBqQ])\?([^?]*)\?=)");
  std::string out;
  size_t last = 0;
  auto begin = std::sregex_iterator(value.begin(), value.end(), encoded_word);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    out.append(value.substr(last, static_cast<size_t>(it->position()) - last));
    std::string encoding = (*it)[2].str();
    std::string payload = (*it)[3].str();
    if (encoding == "B" || encoding == "b") {
      out.append(base64_decode(payload));
    } else {
      out.append(quoted_printable_decode(payload, true));
    }
    last = static_cast<size_t>(it->position() + it->length());
  }
  out.append(value.substr(last));
  return trim(out);
}

void split_headers_body(const std::string& raw, std::vector<std::string>& headers, std::string& body) {
  size_t pos = raw.find("\r\n\r\n");
  size_t sep_len = 4;
  if (pos == std::string::npos) {
    pos = raw.find("\n\n");
    sep_len = 2;
  }
  std::string header_block = (pos == std::string::npos) ? raw : raw.substr(0, pos);
  body = (pos == std::string::npos) ? "" : raw.substr(pos + sep_len);

  std::istringstream iss(header_block);
  std::string line;
  std::string current;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      current += " " + trim(line);
      continue;
    }
    if (!current.empty()) headers.push_back(current);
    current = line;
  }
  if (!current.empty()) headers.push_back(current);
}

std::string extract_part_by_content_type(const std::string& raw, const std::string& content_type) {
  std::string lower = to_lower(raw);
  std::string needle = "content-type: " + content_type;
  size_t pos = lower.find(needle);
  if (pos == std::string::npos) return "";
  size_t body_start = raw.find("\r\n\r\n", pos);
  size_t sep = 4;
  if (body_start == std::string::npos) {
    body_start = raw.find("\n\n", pos);
    sep = 2;
  }
  if (body_start == std::string::npos) return "";
  body_start += sep;
  size_t end = raw.find("\r\n--", body_start);
  if (end == std::string::npos) end = raw.find("\n--", body_start);
  std::string part = raw.substr(body_start, end == std::string::npos ? std::string::npos : end - body_start);
  size_t qp_header = lower.rfind("content-transfer-encoding:", body_start);
  if (qp_header != std::string::npos && qp_header > pos) {
    std::string enc_region = lower.substr(qp_header, body_start - qp_header);
    if (enc_region.find("quoted-printable") != std::string::npos) {
      part = quoted_printable_decode(part, false);
    } else if (enc_region.find("base64") != std::string::npos) {
      part = base64_decode(part);
    }
  }
  return trim(part);
}

std::string strip_html_tags(std::string html) {
  html = std::regex_replace(html, std::regex("<(script|style)[^>]*>[\\s\\S]*?</\\1>", std::regex::icase), " ");
  html = std::regex_replace(html, std::regex("<br\\s*/?>", std::regex::icase), "\n");
  html = std::regex_replace(html, std::regex("</p>", std::regex::icase), "\n");
  html = std::regex_replace(html, std::regex("<[^>]+>", std::regex::icase), " ");
  return trim(html);
}

std::string snippet_from_body(const std::string& body, size_t max_len = 200) {
  std::string s = body;
  for (auto& c : s) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  }
  if (s.size() > max_len) s.resize(max_len);
  return trim(s);
}

std::string sanitize_url(std::string url) {
  while (!url.empty()) {
    char c = url.back();
    if (c == '.' || c == ',' || c == ')' || c == ']' || c == '}' || c == ';') {
      url.pop_back();
      continue;
    }
    break;
  }
  return url;
}

std::string extract_domain(const std::string& url) {
  static const std::regex re(R"(^https?://([^/:?#]+))", std::regex::icase);
  std::smatch m;
  if (!std::regex_search(url, m, re)) return "";
  std::string host = m[1].str();
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return host;
}

double estimate_form_link_confidence(const std::string& url) {
  std::string lower = to_lower(url);
  if (lower.find("forms.yandex.") != std::string::npos) return 0.95;
  if (lower.find("docs.google.com/forms") != std::string::npos) return 0.95;
  if (lower.find("forms.office.com") != std::string::npos) return 0.95;
  if (lower.find("forms.microsoft.com") != std::string::npos) return 0.95;
  if (lower.find("portal.hse.ru/poll") != std::string::npos) return 0.9;
  if (lower.find("lms.hse.ru") != std::string::npos) return 0.7;
  if (lower.find("form") != std::string::npos || lower.find("poll") != std::string::npos) return 0.6;
  return 0.2;
}

void collect_links(const std::string& text, std::vector<link>& result, std::set<std::string>& seen) {
  static const std::regex url_regex(R"((https?://[^\s"'<>]+))", std::regex::icase);

  auto begin = std::sregex_iterator(text.begin(), text.end(), url_regex);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::string url = sanitize_url((*it)[1].str());
    if (url.empty() || !seen.insert(url).second) continue;

    link item;
    item.url = url;
    item.domain = extract_domain(url);
    item.confidence = estimate_form_link_confidence(url);
    result.push_back(std::move(item));
  }
}

std::vector<link> extract_links(const std::string& text, const std::string& html = "") {
  std::vector<link> result;
  std::set<std::string> seen;
  collect_links(text, result, seen);
  collect_links(html, result, seen);
  return result;
}

}  // namespace

class mail_client_imap : public mail_client {
public:
  explicit mail_client_imap(imap_config cfg) : cfg(std::move(cfg)) {}

  std::uint64_t fetch_max_uid() override {
    std::vector<std::string> uids;
    std::string err;
    if (!fetch_uid_list("UID SEARCH ALL", uids, err)) return 0;

    std::uint64_t max_uid = 0;
    for (const auto& uid : uids) {
      max_uid = std::max(max_uid, parse_uint64_or_zero(uid));
    }
    return max_uid;
  }

  std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) override {
    std::vector<message> result;
    std::uint64_t max_uid = fetch_max_uid();
    if (max_uid <= last_seen_uid) return result;

    std::vector<std::string> uids;
    std::string err;
    std::ostringstream search;
    search << "UID SEARCH UID " << (last_seen_uid + 1) << ":" << max_uid;
    if (!fetch_uid_list(search.str(), uids, err)) return result;

    std::sort(uids.begin(), uids.end(), [](const std::string& a, const std::string& b) {
      return parse_uint64_or_zero(a) < parse_uint64_or_zero(b);
    });

    for (const auto& uid : uids) {
      message msg;
      if (fetch_message(uid, msg, err)) {
        result.push_back(msg);
      }
    }
    return result;
  }

private:
  imap_config cfg;

  std::string base_url() const {
    std::string scheme = cfg.tls ? "imaps" : "imap";
    std::ostringstream ss;
    ss << scheme << "://" << cfg.host << ":" << cfg.port << "/" << cfg.folder;
    return ss.str();
  }

  bool perform_request(const std::string& url,
                       const std::string& custom_request,
                       std::string& response,
                       std::string& err) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
      err = "curl init failed";
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, cfg.tls ? CURLUSESSL_ALL : CURLUSESSL_NONE);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!custom_request.empty()) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom_request.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      err = curl_easy_strerror(res);
      curl_easy_cleanup(curl);
      return false;
    }
    curl_easy_cleanup(curl);
    return true;
  }

  bool fetch_uid_list(const std::string& search_command,
                      std::vector<std::string>& uids,
                      std::string& err) const {
    std::string response;
    std::string url = base_url();
    if (!perform_request(url, search_command, response, err)) return false;

    std::istringstream iss(response);
    std::string token;
    while (iss >> token) {
      if (!token.empty() &&
          std::all_of(token.begin(), token.end(),
                      [](unsigned char c) { return std::isdigit(c) != 0; })) {
        uids.push_back(token);
      }
    }
    return true;
  }

  bool fetch_message(const std::string& uid, message& msg, std::string& err) const {
    std::string response;
    std::string url = base_url() + "/;UID=" + uid;
    if (!perform_request(url, "", response, err)) return false;

    std::vector<std::string> headers;
    std::string body;
    split_headers_body(response, headers, body);

    msg.mailbox_id = cfg.mailbox_id;
    msg.uid = uid;
    msg.message_id = get_header(headers, "Message-ID");
    msg.from = get_header(headers, "From");
    msg.to = get_header(headers, "To");
    msg.subject = decode_mime_header(get_header(headers, "Subject"));
    msg.date_iso = get_header(headers, "Date");
    msg.body = body;
    msg.body_text = extract_part_by_content_type(response, "text/plain");
    msg.body_html = extract_part_by_content_type(response, "text/html");
    if (msg.body_text.empty()) {
      msg.body_text = msg.body_html.empty() ? body : strip_html_tags(msg.body_html);
    }
    msg.snippet = snippet_from_body(msg.body_text.empty() ? body : msg.body_text);
    msg.links = extract_links(msg.body_text, msg.body_html);
    if (msg.message_id.empty()) msg.message_id = uid;

    if (cfg.mark_seen) {
      std::string ignore;
      std::string base = base_url();
      perform_request(base, "UID STORE " + uid + " +FLAGS (\\Seen)", ignore, err);
    }
    return true;
  }
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err) {
  if (cfg.host.empty() || cfg.username.empty() || cfg.password.empty()) {
    if (err) *err = "imap config incomplete";
    return nullptr;
  }
  return new mail_client_imap(cfg);
}
