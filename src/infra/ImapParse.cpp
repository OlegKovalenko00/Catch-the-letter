#include "ImapParse.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>


namespace {

std::string trim_str(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  std::size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string base64_decode(const std::string& input) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
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
  for (std::size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (header_mode && c == '_') { out.push_back(' '); continue; }
    if (c == '=' && i + 2 < input.size()) {
      if (input[i+1] == '\r' && input[i+2] == '\n') { i += 2; continue; }
      if (input[i+1] == '\n') { i += 1; continue; }
      int hi = hex_value(input[i+1]);
      int lo = hex_value(input[i+2]);
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

std::string html_entity_decode(std::string text) {
  auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
    std::size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
      s.replace(p, from.size(), to);
      p += to.size();
    }
  };
  replace_all(text, "&amp;",  "&");
  replace_all(text, "&lt;",   "<");
  replace_all(text, "&gt;",   ">");
  replace_all(text, "&quot;", "\"");
  replace_all(text, "&#39;",  "'");
  return text;
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
  if (lower.find("forms.yandex.") != std::string::npos) {
    if (lower.find("/admin/")   != std::string::npos) return 0.0;
    if (lower.find("/answers/") != std::string::npos) return 0.0;
    if (lower.find("/success")  != std::string::npos) return 0.0;
    if (lower.find("/results/") != std::string::npos) return 0.0;
    return 0.95;
  }
  if (lower.find("docs.google.com/forms") != std::string::npos) {
    if (lower.find("/viewanalytics") != std::string::npos) return 0.0;
    if (lower.find("/closedform")    != std::string::npos) return 0.0;
    return 0.95;
  }
  if (lower.find("forms.gle")           != std::string::npos) return 0.95;
  if (lower.find("forms.office.com")    != std::string::npos) return 0.95;
  if (lower.find("forms.microsoft.com") != std::string::npos) return 0.95;
  if (lower.find("portal.hse.ru/poll")  != std::string::npos) return 0.9;
  if (lower.find("lms.hse.ru")          != std::string::npos) return 0.85;
  if (lower.find("smartlms.hse.ru")     != std::string::npos) return 0.85;
  if (lower.find("form") != std::string::npos ||
      lower.find("poll") != std::string::npos) return 0.6;
  return 0.2;
}

void collect_links(const std::string& text,
                   std::vector<message_link>& result,
                   std::set<std::string>& seen) {
  static const std::regex url_regex(R"((https?://[^\s"'<>]+))", std::regex::icase);
  auto begin = std::sregex_iterator(text.begin(), text.end(), url_regex);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    std::string url = sanitize_url(html_entity_decode((*it)[1].str()));
    if (url.empty() || !seen.insert(url).second) continue;
    message_link item;
    item.url        = url;
    item.domain     = extract_domain(url);
    item.confidence = estimate_form_link_confidence(url);
    result.push_back(std::move(item));
  }
}

void collect_href_links(const std::string& html,
                        std::vector<message_link>& result,
                        std::set<std::string>& seen) {
  static const std::regex href_regex(
      R"(href\s*=\s*["'](https?://[^"']+)["'])", std::regex::icase);
  auto begin = std::sregex_iterator(html.begin(), html.end(), href_regex);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    std::string url = sanitize_url(html_entity_decode((*it)[1].str()));
    if (url.empty() || !seen.insert(url).second) continue;
    message_link item;
    item.url        = url;
    item.domain     = extract_domain(url);
    item.confidence = estimate_form_link_confidence(url);
    result.push_back(std::move(item));
  }
}

}


bool looks_like_email_headers(const std::string& s, std::size_t offset) {
  static const char* const kFields[] = {
    "From:", "To:", "Subject:", "Date:", "Message-ID:", "MIME-Version:",
    "Content-Type:", "Received:", "Return-Path:", "Delivered-To:", "X-Mailer:"
  };
  if (offset >= s.size()) return false;
  std::string hay = s.substr(offset, std::min<std::size_t>(1024, s.size() - offset));
  for (const char* f : kFields)
    if (hay.find(f) != std::string::npos) return true;
  return false;
}

std::string imap_extract_literal(const std::string& response) {
  for (std::size_t pos = 0; pos < response.size(); ) {
    auto brace = response.find('{', pos);
    if (brace == std::string::npos) break;
    auto end_brace = response.find('}', brace);
    if (end_brace == std::string::npos) break;


    if (end_brace <= brace + 1) { pos = end_brace + 1; continue; }

    bool all_digits = true;
    for (std::size_t i = brace + 1; i < end_brace; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(response[i]))) {
        all_digits = false; break;
      }
    }
    if (!all_digits) { pos = end_brace + 1; continue; }

    std::size_t size = 0;
    try {
      size = std::stoull(response.substr(brace + 1, end_brace - brace - 1));
    } catch (...) { pos = end_brace + 1; continue; }

    if (size == 0) { pos = end_brace + 1; continue; }


    std::size_t lf = response.find('\n', end_brace);
    if (lf == std::string::npos) { pos = end_brace + 1; continue; }
    std::size_t data_start = lf + 1;
    if (data_start >= response.size()) { pos = end_brace + 1; continue; }

    if (data_start + size <= response.size()) {

      return response.substr(data_start, size);
    }


    if (looks_like_email_headers(response, data_start))
      return response.substr(data_start);

    pos = end_brace + 1;
  }
  return "";
}

std::string strip_imap_framing(const std::string& response) {
  std::istringstream iss(response);
  std::string line, result;
  bool in_email = false;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (!in_email) {
      bool is_imap_line =
          (line.size() >= 2 && line[0] == '*' && line[1] == ' ') ||
          (line.rfind("A001 ", 0) == 0) ||
          (line.rfind("A002 ", 0) == 0) ||
          (line.rfind("+ ", 0) == 0);
      if (is_imap_line) continue;


      if (line.size() >= 3 && line[0] == '{' && line.back() == '}') {
        bool digits = true;
        for (std::size_t i = 1; i + 1 < line.size(); ++i)
          if (!std::isdigit(static_cast<unsigned char>(line[i]))) { digits = false; break; }
        if (digits) continue;
      }

      in_email = true;
    }


    if (in_email && (line.rfind("A001 ", 0) == 0 || line.rfind("A002 ", 0) == 0))
      break;

    result += line + "\r\n";
  }
  return result;
}

bool is_incomplete_literal(const std::string& response) {
  for (std::size_t pos = 0; pos < response.size(); ) {
    auto brace = response.find('{', pos);
    if (brace == std::string::npos) break;
    auto end_brace = response.find('}', brace);
    if (end_brace == std::string::npos) break;

    if (end_brace <= brace + 1) { pos = end_brace + 1; continue; }

    bool all_digits = true;
    for (std::size_t i = brace + 1; i < end_brace; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(response[i]))) {
        all_digits = false; break;
      }
    }
    if (!all_digits) { pos = end_brace + 1; continue; }

    std::size_t size = 0;
    try {
      size = std::stoull(response.substr(brace + 1, end_brace - brace - 1));
    } catch (...) { pos = end_brace + 1; continue; }

    if (size == 0) { pos = end_brace + 1; continue; }


    std::size_t after = end_brace + 1;
    if (after < response.size() && response[after] == '\r') after++;
    if (after >= response.size() || response[after] != '\n') {
      pos = end_brace + 1; continue;
    }
    std::size_t data_start = after + 1;

    if (data_start + size > response.size()) return true;

    pos = end_brace + 1;
  }
  return false;
}

std::string imap_extract_raw(const std::string& imap_response,
                              message_parse_diagnostics& diag) {
  diag.response_size   = imap_response.size();
  diag.response_prefix = imap_response.substr(
      0, std::min<std::size_t>(300, imap_response.size()));


  std::string raw = imap_extract_literal(imap_response);
  if (!raw.empty()) {
    diag.strategy        = "literal";
    diag.literal_found   = true;
    diag.literal_size_ok = true;
    diag.extracted_size  = raw.size();
    return raw;
  }


  if (looks_like_email_headers(imap_response)) {
    diag.strategy       = "bare";
    diag.extracted_size = imap_response.size();
    return imap_response;
  }


  {
    std::string stripped = strip_imap_framing(imap_response);
    if (!stripped.empty() && looks_like_email_headers(stripped)) {
      diag.strategy       = "stripped";
      diag.extracted_size = stripped.size();
      return stripped;
    }
  }


  diag.strategy       = "raw_fallback";
  diag.extracted_size = imap_response.size();
  return imap_response;
}

void split_headers_body(const std::string& raw,
                        std::vector<std::string>& headers,
                        std::string& body) {
  std::size_t pos     = raw.find("\r\n\r\n");
  std::size_t sep_len = 4;
  if (pos == std::string::npos) { pos = raw.find("\n\n"); sep_len = 2; }

  std::string header_block = (pos == std::string::npos) ? raw : raw.substr(0, pos);
  body = (pos == std::string::npos) ? "" : raw.substr(pos + sep_len);

  std::istringstream iss(header_block);
  std::string line, current;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      current += " " + trim_str(line);
      continue;
    }
    if (!current.empty()) headers.push_back(current);
    current = line;
  }
  if (!current.empty()) headers.push_back(current);
}

std::string get_header(const std::vector<std::string>& headers,
                       const std::string& name) {
  std::string needle = to_lower(name);
  for (const auto& h : headers) {
    auto pos = h.find(':');
    if (pos == std::string::npos) continue;
    std::string key = to_lower(trim_str(h.substr(0, pos)));
    if (key == needle) return trim_str(h.substr(pos + 1));
  }
  return "";
}

std::string decode_mime_header(const std::string& value) {
  static const std::regex encoded_word(R"(=\?([^?]+)\?([bBqQ])\?([^?]*)\?=)");
  std::string out;
  std::size_t last = 0;
  auto begin = std::sregex_iterator(value.begin(), value.end(), encoded_word);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    out.append(value.substr(last, static_cast<std::size_t>(it->position()) - last));
    std::string enc     = (*it)[2].str();
    std::string payload = (*it)[3].str();
    if (enc == "B" || enc == "b")
      out.append(base64_decode(payload));
    else
      out.append(quoted_printable_decode(payload, true));
    last = static_cast<std::size_t>(it->position() + it->length());
  }
  out.append(value.substr(last));
  return trim_str(out);
}

std::string extract_part_by_content_type(const std::string& raw,
                                          const std::string& content_type) {
  std::string lower  = to_lower(raw);
  std::string needle = "content-type: " + content_type;
  std::size_t pos    = lower.find(needle);
  if (pos == std::string::npos) return "";

  std::size_t body_start = raw.find("\r\n\r\n", pos);
  std::size_t sep        = 4;
  if (body_start == std::string::npos) { body_start = raw.find("\n\n", pos); sep = 2; }
  if (body_start == std::string::npos) return "";
  body_start += sep;

  std::size_t end = raw.find("\r\n--", body_start);
  if (end == std::string::npos) end = raw.find("\n--", body_start);
  std::string part = raw.substr(body_start,
      end == std::string::npos ? std::string::npos : end - body_start);

  std::size_t qp_hdr = lower.rfind("content-transfer-encoding:", body_start);
  if (qp_hdr != std::string::npos && qp_hdr > pos) {
    std::string enc_region = lower.substr(qp_hdr, body_start - qp_hdr);
    if (enc_region.find("quoted-printable") != std::string::npos)
      part = quoted_printable_decode(part, false);
    else if (enc_region.find("base64") != std::string::npos)
      part = base64_decode(part);
  }
  return trim_str(part);
}

std::string strip_html_tags(const std::string& html_in) {
  std::string html = html_in;
  html = std::regex_replace(html, std::regex("<(script|style)[^>]*>[\\s\\S]*?</\\1>",
                                             std::regex::icase), " ");
  html = std::regex_replace(html, std::regex("<br\\s*/?>",  std::regex::icase), "\n");
  html = std::regex_replace(html, std::regex("</p>",        std::regex::icase), "\n");
  html = std::regex_replace(html, std::regex("<[^>]+>",     std::regex::icase), " ");
  return trim_str(html);
}

std::string snippet_from_body(const std::string& body, std::size_t max_len) {
  std::string s = body;
  for (auto& c : s)
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  if (s.size() > max_len) s.resize(max_len);
  return trim_str(s);
}

std::vector<message_link> extract_links(const std::string& text,
                                         const std::string& html) {
  std::vector<message_link> result;
  std::set<std::string> seen;
  collect_links(text, result, seen);
  collect_links(html, result, seen);
  collect_href_links(html, result, seen);
  return result;
}

std::vector<attachment> extract_attachment_metadata(const std::string& raw) {
  std::vector<attachment> result;
  std::string lower = to_lower(raw);
  std::size_t search_from = 0;
  while (true) {
    std::size_t disp_pos = lower.find("content-disposition:", search_from);
    if (disp_pos == std::string::npos) break;
    std::size_t line_end = lower.find('\n', disp_pos);
    if (line_end == std::string::npos) break;
    std::string disp_line  = raw.substr(disp_pos, line_end - disp_pos);
    std::string disp_lower = to_lower(disp_line);
    search_from = line_end + 1;

    bool is_att    = disp_lower.find("attachment") != std::string::npos;
    bool is_inline = disp_lower.find("inline")     != std::string::npos;
    if (!is_att && !is_inline) continue;

    attachment att;
    att.disposition = is_att ? "attachment" : "inline";

    static const std::regex fn_re(R"(filename\*?=\"?([^\";\r\n]+)\"?)",
                                   std::regex::icase);
    std::smatch fn_m;
    if (std::regex_search(disp_line, fn_m, fn_re)) {
      att.filename = trim_str(fn_m[1].str());
      att.filename = decode_mime_header(att.filename);
    }

    std::size_t back_start  = disp_pos > 800 ? disp_pos - 800 : 0;
    std::string back_lower  = lower.substr(back_start, disp_pos - back_start);
    std::size_t ct_rel      = back_lower.rfind("content-type:");
    if (ct_rel != std::string::npos) {
      std::size_t ct_abs  = back_start + ct_rel;
      std::size_t ct_end  = lower.find('\n', ct_abs);
      std::string ct_line = ct_end != std::string::npos
          ? raw.substr(ct_abs, ct_end - ct_abs) : raw.substr(ct_abs);
      auto semi           = ct_line.find(';');
      att.mime_type       = to_lower(trim_str(ct_line.substr(14,
          semi == std::string::npos ? std::string::npos : semi - 14)));

      if (att.mime_type.rfind("image/", 0) == 0) att.safe_to_preview = true;
      if (att.mime_type == "application/pdf")    att.safe_to_preview = true;

      std::size_t cid_pos = back_lower.rfind("content-id:");
      if (cid_pos != std::string::npos) {
        std::size_t cid_abs  = back_start + cid_pos;
        std::size_t cid_end  = lower.find('\n', cid_abs);
        std::string cid_line = cid_end != std::string::npos
            ? raw.substr(cid_abs, cid_end - cid_abs) : raw.substr(cid_abs);
        std::string cid_val  = trim_str(cid_line.substr(11));
        if (!cid_val.empty() && cid_val.front() == '<') cid_val = cid_val.substr(1);
        if (!cid_val.empty() && cid_val.back()  == '>') cid_val.pop_back();
        att.content_id = cid_val;
      }
    }

    if (!att.filename.empty() || !att.content_id.empty() || is_att)
      result.push_back(std::move(att));
  }
  return result;
}
