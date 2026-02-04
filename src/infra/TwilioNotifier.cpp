#include "TwilioNotifier.h"

#include <curl/curl.h>

#include <sstream>
#include <string>

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string url_encode(CURL* curl, const std::string& s) {
  char* enc = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
  if (!enc) return s;
  std::string out(enc);
  curl_free(enc);
  return out;
}

std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

bool perform_twilio_post(const std::string& url,
                         const std::string& user,
                         const std::string& pass,
                         const std::string& body,
                         std::string& err) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
  curl_easy_setopt(curl, CURLOPT_PASSWORD, pass.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    err = curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return false;
  }
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (code < 200 || code >= 300) {
    std::ostringstream ss;
    ss << "twilio http " << code;
    err = ss.str();
    return false;
  }
  err.clear();
  return true;
}

}

twilio_notifier::twilio_notifier(twilio_config cfg) : cfg(std::move(cfg)) {}

bool twilio_notifier::send_sms(const message&, const std::string& text, std::string& err) {
  if (!cfg.enabled) {
    err = "twilio disabled";
    return false;
  }
  if (cfg.account_sid.empty() || cfg.auth_token.empty() ||
      cfg.from_number.empty() || cfg.sms_to.empty()) {
    err = "twilio sms config incomplete";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string url = "https://api.twilio.com/2010-04-01/Accounts/" +
                    cfg.account_sid + "/Messages.json";
  std::string body = "From=" + url_encode(curl, cfg.from_number) +
                     "&To=" + url_encode(curl, cfg.sms_to) +
                     "&Body=" + url_encode(curl, text);

  curl_easy_cleanup(curl);
  return perform_twilio_post(url, cfg.account_sid, cfg.auth_token, body, err);
}

bool twilio_notifier::make_call(const message&, const std::string& text, std::string& err) {
  if (!cfg.enabled) {
    err = "twilio disabled";
    return false;
  }
  if (cfg.account_sid.empty() || cfg.auth_token.empty() ||
      cfg.from_number.empty() || cfg.voice_to.empty()) {
    err = "twilio voice config incomplete";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string url = "https://api.twilio.com/2010-04-01/Accounts/" +
                    cfg.account_sid + "/Calls.json";
  std::string twiml = "<Response><Say>" + xml_escape(text) + "</Say></Response>";
  std::string body = "From=" + url_encode(curl, cfg.from_number) +
                     "&To=" + url_encode(curl, cfg.voice_to) +
                     "&Twiml=" + url_encode(curl, twiml);

  curl_easy_cleanup(curl);
  return perform_twilio_post(url, cfg.account_sid, cfg.auth_token, body, err);
}
