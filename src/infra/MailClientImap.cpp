#include "MailClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
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

std::string snippet_from_body(const std::string& body, size_t max_len = 200) {
  std::string s = body;
  for (auto& c : s) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  }
  if (s.size() > max_len) s.resize(max_len);
  return trim(s);
}

}  // namespace

class mail_client_imap : public mail_client {
public:
  explicit mail_client_imap(imap_config cfg) : cfg(std::move(cfg)) {}

  std::vector<message> fetch_unseen() override {
    std::vector<message> result;
    std::vector<std::string> uids;
    std::string err;
    if (!fetch_uid_list(uids, err)) return result;

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

  bool fetch_uid_list(std::vector<std::string>& uids, std::string& err) const {
    std::string response;
    std::string url = base_url();
    if (!perform_request(url, "UID SEARCH UNSEEN", response, err)) return false;

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

    msg.uid = uid;
    msg.message_id = get_header(headers, "Message-ID");
    msg.from = get_header(headers, "From");
    msg.to = get_header(headers, "To");
    msg.subject = get_header(headers, "Subject");
    msg.date_iso = get_header(headers, "Date");
    msg.body = body;
    msg.snippet = snippet_from_body(body);
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
