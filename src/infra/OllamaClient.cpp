#include "LlmClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <utility>

using nlohmann::json;

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

class ollama_client final : public llm_client {
public:
  explicit ollama_client(llm_config cfg) : cfg(std::move(cfg)), fallback(make_noop_llm_client()) {}

  email_analysis analyze_email(const message& msg) override {
    json response;
    std::string err;
    if (!chat(build_email_prompt(msg), response, err)) return fallback->analyze_email(msg);

    try {
      std::string content = response.at("message").at("content").get<std::string>();
      json parsed = json::parse(content);
      email_analysis out = fallback->analyze_email(msg);
      std::string kind = parsed.value("kind", "unknown");
      if (kind == "important_notification") out.kind = message_kind::important_notification;
      else if (kind == "form_request") out.kind = message_kind::form_request;
      else if (kind == "auth_required") out.kind = message_kind::auth_required;
      else if (kind == "ignored") out.kind = message_kind::ignored;
      else out.kind = message_kind::unknown;
      out.confidence = parsed.value("confidence", out.confidence);
      out.summary = parsed.value("summary", out.summary);
      out.user_action_required = parsed.value("user_action_required", out.user_action_required);
      if (parsed.contains("form_links") && parsed["form_links"].is_array()) {
        out.form_links.clear();
        for (const auto& item : parsed["form_links"]) {
          link l;
          l.url = item.value("url", "");
          l.domain = item.value("domain", "");
          l.confidence = item.value("confidence", 0.0);
          if (!l.url.empty()) out.form_links.push_back(l);
        }
      }
      return out;
    } catch (...) {
      return fallback->analyze_email(msg);
    }
  }

  std::vector<form_field> map_fields(const message& msg,
                                     const form_snapshot& form,
                                     const user_profile& profile) override {
    json fields = json::array();
    for (const auto& f : form.fields) {
      fields.push_back({{"id", f.id}, {"label", f.label}, {"type", f.type}, {"options", f.options}});
    }

    json profile_json;
    for (const auto& [key, value] : profile.values) {
      if (cfg.privacy_mode == "safe" &&
          (key.find("passport") != std::string::npos || key.find("snils") != std::string::npos ||
           key.find("birth") != std::string::npos || key.find("password") != std::string::npos)) {
        continue;
      }
      profile_json[key] = value;
    }

    std::ostringstream prompt;
    prompt << "Сопоставь поля формы с профилем. Верни только JSON вида "
              "{\"fields\":[{\"field_id\":\"...\",\"mapped_profile_key\":\"...\","
              "\"suggested_value\":\"...\",\"confidence\":0.0,\"requires_user_input\":true}]}.\n"
              "Не подставляй чувствительные документы. Если поле про мнение/оценку/комментарий, "
              "requires_user_input=true.\n"
           << "Тема письма: " << msg.subject << "\n"
           << "Поля: " << fields.dump() << "\n"
           << "Профиль: " << profile_json.dump();

    json response;
    std::string err;
    auto fallback_fields = fallback->map_fields(msg, form, profile);
    if (!chat(prompt.str(), response, err)) return fallback_fields;

    try {
      std::string content = response.at("message").at("content").get<std::string>();
      json parsed = json::parse(content);
      if (!parsed.contains("fields") || !parsed["fields"].is_array()) return fallback_fields;
      for (const auto& item : parsed["fields"]) {
        std::string id = item.value("field_id", "");
        for (auto& field : fallback_fields) {
          if (field.id != id) continue;
          field.mapped_profile_key = item.value("mapped_profile_key", field.mapped_profile_key);
          field.value = item.value("suggested_value", field.value);
          field.confidence = item.value("confidence", field.confidence);
          field.requires_user_input = item.value("requires_user_input", field.requires_user_input);
        }
      }
      return fallback_fields;
    } catch (...) {
      return fallback_fields;
    }
  }

private:
  std::string build_email_prompt(const message& msg) const {
    json links = json::array();
    for (const auto& item : msg.links) {
      links.push_back({{"url", item.url}, {"domain", item.domain}, {"confidence", item.confidence}});
    }

    std::ostringstream prompt;
    prompt << "Проанализируй письмо. Верни только JSON: "
              "{\"kind\":\"ignored|important_notification|form_request|auth_required|unknown\","
              "\"confidence\":0.0,\"summary\":\"...\",\"user_action_required\":true,"
              "\"form_links\":[{\"url\":\"...\",\"domain\":\"...\",\"confidence\":0.0}]}.\n"
           << "Не принимай решений об отправке форм.\n"
           << "От: " << msg.from << "\n"
           << "Тема: " << msg.subject << "\n"
           << "Текст: " << msg.snippet << "\n"
           << "Ссылки: " << links.dump();
    return prompt.str();
  }

  bool chat(const std::string& prompt, json& response, std::string& err) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
      err = "curl init failed";
      return false;
    }

    json req = {
        {"model", cfg.model},
        {"stream", false},
        {"messages", json::array({
            {{"role", "system"}, {"content", "Ты локальный помощник. Возвращай только JSON."}},
            {{"role", "user"}, {"content", prompt}}
        })}
    };
    std::string payload = req.dump();
    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, cfg.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

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
      ss << "ollama HTTP " << code;
      err = ss.str();
      return false;
    }
    try {
      response = json::parse(body);
      return true;
    } catch (const std::exception& e) {
      err = std::string("ollama JSON parse failed: ") + e.what();
      return false;
    }
  }

  llm_config cfg;
  std::unique_ptr<llm_client> fallback;
};

}  // namespace

std::unique_ptr<llm_client> make_ollama_client(const llm_config& cfg) {
  return std::make_unique<ollama_client>(cfg);
}
