#include "MailClient.h"
#include "ImapParse.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct curl_global_guard {
  curl_global_guard()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~curl_global_guard() { curl_global_cleanup(); }
};

static curl_global_guard curl_guard;

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::uint64_t parse_uint64_or_zero(const std::string& text) {
  try {
    std::size_t pos = 0;
    std::uint64_t value = std::stoull(text, &pos);
    return pos == text.size() ? value : 0;
  } catch (...) {
    return 0;
  }
}


std::string parse_uid_validity(const std::string& response) {
  static const std::regex re(R"(\[UIDVALIDITY\s+(\d+)\])", std::regex::icase);
  std::smatch m;
  if (std::regex_search(response, m, re)) return m[1].str();
  static const std::regex re2(R"(UIDVALIDITY\s+(\d+))", std::regex::icase);
  if (std::regex_search(response, m, re2)) return m[1].str();
  return "";
}

}

class mail_client_imap : public mail_client {
public:
  explicit mail_client_imap(imap_config cfg) : cfg(std::move(cfg)) {}

  friend imap_test_result test_imap_mailbox(const imap_config& cfg);

  std::uint64_t fetch_max_uid() override {
    std::vector<std::string> uids;
    std::string err;
    if (!fetch_uid_list("UID SEARCH ALL", uids, err)) return 0;
    std::uint64_t max_uid = 0;
    for (const auto& uid : uids)
      max_uid = std::max(max_uid, parse_uint64_or_zero(uid));
    return max_uid;
  }


  std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) override {
    auto r = fetch_after_uid_result(last_seen_uid);
    return std::move(r.messages);
  }

  mail_fetch_result fetch_after_uid_result(std::uint64_t last_seen_uid) override {
    mail_fetch_result result;
    result.mailbox_id    = cfg.mailbox_id;
    result.last_seen_uid = last_seen_uid;
    result.uid_validity  = fetch_uid_validity();

    std::vector<std::string> uids;
    std::string err;

    std::ostringstream search;
    search << "UID SEARCH UID " << (last_seen_uid + 1) << ":*";
    std::string search_cmd = search.str();
    std::cout << "[mail] imap search cmd=" << search_cmd
              << " mailbox=" << cfg.mailbox_id << std::endl;

    if (!fetch_uid_list(search_cmd, uids, err)) {
      std::cout << "[mail] imap search failed err=" << err << std::endl;
      result.ok    = false;
      result.error = err.empty() ? "imap search failed" : err;
      return result;
    }


    uids.erase(std::remove_if(uids.begin(), uids.end(), [last_seen_uid](const std::string& u) {
      return parse_uint64_or_zero(u) <= last_seen_uid;
    }), uids.end());
    std::sort(uids.begin(), uids.end(), [](const std::string& a, const std::string& b) {
      return parse_uint64_or_zero(a) < parse_uint64_or_zero(b);
    });

    result.searched_uids = uids;
    std::cout << "[mail] imap new uids count=" << uids.size()
              << " last_seen_uid=" << last_seen_uid << std::endl;

    for (const auto& uid : uids) {
      std::cout << "[mail] fetch uid=" << uid << " mailbox=" << cfg.mailbox_id << std::endl;
      message msg;
      std::string fetch_err;
      if (fetch_message(uid, msg, fetch_err)) {
        result.fetched_uids.push_back(uid);
        std::cout << "[mail] fetched uid=" << uid
                  << " strategy=" << msg.parse_strategy
                  << " from=" << msg.from
                  << " subject=" << msg.subject
                  << " links=" << msg.links.size()
                  << " parse_suspect=" << msg.parse_suspect << std::endl;
        if (msg.parse_suspect) {
          result.parse_failed_uids.push_back(uid);
        } else {
          std::uint64_t uid_n = parse_uint64_or_zero(uid);
          if (uid_n > result.max_seen_uid) result.max_seen_uid = uid_n;
          result.messages.push_back(std::move(msg));
        }
      } else {
        std::cout << "[mail] fetch failed uid=" << uid << " err=" << fetch_err << std::endl;
        result.failed_uids.push_back(uid);
      }
    }
    return result;
  }

  std::vector<message> fetch_last_n(int n) override {
    if (n <= 0) return {};
    std::vector<std::string> all_uids;
    std::string err;
    std::cout << "[mail] fetch_last_n n=" << n << " mailbox=" << cfg.mailbox_id << std::endl;
    if (!fetch_uid_list("UID SEARCH ALL", all_uids, err)) {
      std::cout << "[mail] fetch_last_n search failed err=" << err << std::endl;
      return {};
    }
    std::sort(all_uids.begin(), all_uids.end(), [](const std::string& a, const std::string& b) {
      return parse_uint64_or_zero(a) < parse_uint64_or_zero(b);
    });
    if (static_cast<int>(all_uids.size()) > n)
      all_uids.erase(all_uids.begin(), all_uids.end() - n);
    std::vector<message> result;
    for (const auto& uid : all_uids) {
      message msg;
      std::string fetch_err;
      if (fetch_message(uid, msg, fetch_err)) result.push_back(std::move(msg));
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
    curl_easy_setopt(curl, CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME,      cfg.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD,      cfg.password.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL,       cfg.tls ? CURLUSESSL_ALL : CURLUSESSL_NONE);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!custom_request.empty())
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom_request.c_str());
    char error_buffer[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,  error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,     1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,    &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,      30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      err = error_buffer[0]
          ? std::string(error_buffer)
          : std::string(curl_easy_strerror(res));
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
                      [](unsigned char c) { return std::isdigit(c) != 0; }))
        uids.push_back(token);
    }
    return true;
  }


  bool fetch_message(const std::string& uid, message& msg, std::string& err,
                     std::size_t* raw_size_out = nullptr) const {
    std::string response;


    std::string url = base_url() + "/;UID=" + uid;
    if (!perform_request(url, "", response, err)) return false;

    if (raw_size_out) *raw_size_out = response.size();


    if (is_incomplete_literal(response)) {
      err = "incomplete_literal: {N} marker present but body truncated (response_size="
            + std::to_string(response.size()) + ")";
      return false;
    }


    message_parse_diagnostics diag;
    std::string raw = imap_extract_raw(response, diag);

    std::vector<std::string> headers;
    std::string body;
    split_headers_body(raw, headers, body);
    diag.headers_count = headers.size();
    diag.body_size     = body.size();

    msg.mailbox_id  = cfg.mailbox_id;
    msg.provider    = cfg.provider;
    msg.uid         = uid;
    msg.message_id  = get_header(headers, "Message-ID");
    msg.from        = get_header(headers, "From");
    msg.to          = get_header(headers, "To");
    msg.subject     = decode_mime_header(get_header(headers, "Subject"));
    msg.date_iso    = get_header(headers, "Date");
    msg.body        = body;
    msg.body_text   = extract_part_by_content_type(raw, "text/plain");
    msg.body_html   = extract_part_by_content_type(raw, "text/html");
    if (msg.body_text.empty())
      msg.body_text = msg.body_html.empty() ? body : strip_html_tags(msg.body_html);
    msg.snippet     = snippet_from_body(msg.body_text.empty() ? body : msg.body_text);
    msg.links       = extract_links(msg.body_text, msg.body_html);
    if (msg.message_id.empty()) msg.message_id = uid;

    msg.attachments = extract_attachment_metadata(raw);
    for (std::size_t i = 0; i < msg.attachments.size(); ++i)
      msg.attachments[i].part_id = std::to_string(i + 2);

    msg.parse_strategy = diag.strategy;


    if (msg.subject.empty() && msg.from.empty() && msg.body_text.empty()) {
      msg.parse_suspect = true;
      std::cout << "[mail] parse_suspect uid=" << uid
                << " strategy=" << diag.strategy
                << " response_size=" << diag.response_size
                << " extracted=" << diag.extracted_size
                << " headers=" << diag.headers_count
                << " body=" << diag.body_size
                << "\n[mail] response_prefix="
                << diag.response_prefix.substr(
                       0, std::min<std::size_t>(200, diag.response_prefix.size()))
                << std::endl;
    }

    return true;
  }

  std::string fetch_uid_validity() override {
    std::string response;
    std::string err;
    std::string url = base_url();
    if (!perform_request(url, "STATUS " + cfg.folder + " (UIDVALIDITY)", response, err))
      return "";
    return parse_uid_validity(response);
  }

  void mark_message_seen(const std::string& uid) override {
    std::string ignore_response;
    std::string ignore_err;
    perform_request(base_url(), "UID STORE " + uid + " +FLAGS (\\Seen)",
                    ignore_response, ignore_err);
  }
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err) {
  if (cfg.host.empty() || cfg.username.empty() || cfg.password.empty()) {
    if (err) *err = "imap config incomplete";
    return nullptr;
  }
  return new mail_client_imap(cfg);
}

imap_test_result test_imap_mailbox(const imap_config& cfg) {
  imap_test_result result;
  if (cfg.host.empty() || cfg.username.empty() || cfg.password.empty()) {
    result.error = "imap username/password are not configured";
    return result;
  }
  mail_client_imap client(cfg);
  std::vector<std::string> uids;
  std::string err;
  if (!client.fetch_uid_list("UID SEARCH ALL", uids, err)) {
    result.error = err.empty() ? "imap connection failed" : err;
    return result;
  }
  result.reachable  = true;
  result.auth_ok    = true;
  result.folder_ok  = true;
  for (const auto& uid : uids)
    result.max_uid = std::max(result.max_uid, parse_uint64_or_zero(uid));

  result.fetch_mode = "uid_url";


  if (result.max_uid > 0) {
    message msg;
    std::string fetch_err;
    std::size_t raw_size = 0;
    if (!client.fetch_message(std::to_string(result.max_uid), msg, fetch_err, &raw_size)) {
      result.response_size      = static_cast<int>(raw_size);
      result.incomplete_literal = fetch_err.find("incomplete_literal") != std::string::npos;
      result.error = "UID SEARCH OK but FETCH uid=" + std::to_string(result.max_uid) +
                     " failed: " + (fetch_err.empty() ? "unknown" : fetch_err);
    } else {
      result.response_size          = static_cast<int>(raw_size);
      result.incomplete_literal     = false;
      result.latest_fetch_ok        = true;
      result.latest_subject_present = !msg.subject.empty();
      result.latest_from_present    = !msg.from.empty();
      result.latest_body_present    = !msg.body_text.empty();
      result.latest_body_length     = static_cast<int>(msg.body_text.size());
      result.links_count            = static_cast<int>(msg.links.size());
      result.parse_suspect          = msg.parse_suspect;
      if (msg.parse_suspect && result.error.empty()) {
        result.error = "FETCH succeeded but message has empty subject/from/body — "
                       "IMAP parse suspect (strategy=" + msg.parse_strategy + ")";
      }
    }
  }
  return result;
}
