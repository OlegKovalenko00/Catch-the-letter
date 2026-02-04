#include "TelegramNotifier.h"

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

}  // namespace

class telegram_notifier_http final : public telegram_notifier {
public:
  explicit telegram_notifier_http(telegram_config cfg) : cfg(std::move(cfg)) {}

  bool notify(const message&, const std::string& text, std::string& err) override {
    if (!cfg.enabled) {
      err = "telegram disabled";
      return false;
    }
    if (cfg.bot_token.empty() || cfg.chat_id.empty()) {
      err = "telegram config incomplete";
      return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
      err = "curl init failed";
      return false;
    }

    std::string url = "https://api.telegram.org/bot" + cfg.bot_token + "/sendMessage";
    std::string body = "chat_id=" + url_encode(curl, cfg.chat_id) +
                       "&text=" + url_encode(curl, text);
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
      ss << "telegram http " << code;
      err = ss.str();
      return false;
    }

    err.clear();
    return true;
  }

private:
  telegram_config cfg;
};

telegram_notifier* make_telegram_notifier_http(const telegram_config& cfg, std::string* err) {
  if (cfg.bot_token.empty() || cfg.chat_id.empty()) {
    if (err) *err = "telegram config incomplete";
    return nullptr;
  }
  return new telegram_notifier_http(cfg);
}
