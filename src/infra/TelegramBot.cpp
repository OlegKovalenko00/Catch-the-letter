#include "TelegramBot.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>

using nlohmann::json;

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string chat_id_from_json(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());
  if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
  return "";
}

}  // namespace

telegram_bot::telegram_bot(telegram_config cfg) : cfg(std::move(cfg)) {}

bool telegram_bot::enabled() const {
  return cfg.enabled && !cfg.bot_token.empty() && !cfg.chat_id.empty();
}

bool telegram_bot::request(const std::string& method,
                           const json& payload,
                           json& response,
                           std::string& err) const {
  if (!enabled()) {
    err = "telegram disabled";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string url = "https://api.telegram.org/bot" + cfg.bot_token + "/" + method;
  std::string body;
  std::string request_body = payload.dump();
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  if (!cfg.proxy_url.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, cfg.proxy_url.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = curl_easy_strerror(rc);
    return false;
  }
  if (code < 200 || code >= 300) {
    std::ostringstream ss;
    ss << "telegram HTTP " << code;
    err = ss.str();
    return false;
  }

  try {
    response = json::parse(body.empty() ? "{}" : body);
  } catch (const std::exception& e) {
    err = std::string("telegram JSON parse failed: ") + e.what();
    return false;
  }
  if (!response.value("ok", false)) {
    err = response.value("description", "telegram ok=false");
    return false;
  }
  err.clear();
  return true;
}

bool telegram_bot::send_message(const std::string& text,
                                const std::vector<std::vector<telegram_button>>& inline_keyboard,
                                std::string& err) const {
  if (!enabled()) {
    err = "telegram disabled";
    return false;
  }

  json payload = {{"chat_id", cfg.chat_id}, {"text", text}};
  if (!inline_keyboard.empty()) {
    json rows = json::array();
    for (const auto& row : inline_keyboard) {
      json buttons = json::array();
      for (const auto& button : row) {
        buttons.push_back({{"text", button.text}, {"callback_data", button.callback_data}});
      }
      rows.push_back(std::move(buttons));
    }
    payload["reply_markup"] = {{"inline_keyboard", rows}};
  }
  json response;
  return request("sendMessage", payload, response, err);
}

bool telegram_bot::send_message(const std::string& text, std::string& err) const {
  return send_message(text, {}, err);
}

std::vector<telegram_update> telegram_bot::get_updates(long long offset, std::string& err) const {
  json payload = {{"timeout", 10}, {"offset", offset}};
  json response;
  if (!request("getUpdates", payload, response, err)) return {};

  std::vector<telegram_update> updates;
  if (!response.contains("result") || !response["result"].is_array()) return updates;
  for (const auto& item : response["result"]) {
    telegram_update update;
    update.update_id = item.value("update_id", 0LL);
    if (item.contains("message")) {
      const auto& msg = item["message"];
      update.chat_id = chat_id_from_json(msg.value("chat", json::object()).value("id", json("")));
      update.text = msg.value("text", "");
      update.message_id = msg.value("message_id", 0LL);
    }
    if (item.contains("callback_query")) {
      const auto& cb = item["callback_query"];
      update.callback_query_id = cb.value("id", "");
      update.callback_data = cb.value("data", "");
      if (cb.contains("message")) {
        const auto& msg = cb["message"];
        update.chat_id = chat_id_from_json(msg.value("chat", json::object()).value("id", json("")));
        update.message_id = msg.value("message_id", 0LL);
      }
    }
    if (!update.chat_id.empty()) updates.push_back(std::move(update));
  }
  return updates;
}

bool telegram_bot::answer_callback_query(const std::string& callback_query_id,
                                         const std::string& text,
                                         std::string& err) const {
  if (callback_query_id.empty()) {
    err.clear();
    return true;
  }
  json response;
  json payload = {{"callback_query_id", callback_query_id}};
  if (!text.empty()) payload["text"] = text;
  return request("answerCallbackQuery", payload, response, err);
}
