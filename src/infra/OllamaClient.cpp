#include "LlmClient.h"

#include "../app/FormUnderstandingEngine.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
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

double clamp01(double value) {
  if (value < 0.0) return 0.0;
  if (value > 1.0) return 1.0;
  return value;
}

bool message_has_link(const message& msg, const std::string& url, message_link& out) {
  for (const auto& item : msg.links) {
    if (item.url == url) {
      out = item;
      return true;
    }
  }
  return false;
}

bool is_sensitive_key(const std::string& key) {
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower.find("passport") != std::string::npos ||
         lower.find("snils") != std::string::npos ||
         lower.find("birth") != std::string::npos ||
         lower.find("password") != std::string::npos ||
         lower.find("token") != std::string::npos ||
         lower.find("secret") != std::string::npos ||
         lower.find("code") != std::string::npos ||
         lower.find("cookie") != std::string::npos;
}

bool is_opinion_field(const form_field& field) {
  std::string text = field.label + " " + field.id + " " + field.type;
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text.find("opinion") != std::string::npos ||
         text.find("comment") != std::string::npos ||
         text.find("rating") != std::string::npos ||
         text.find("feedback") != std::string::npos ||
         text.find("оцен") != std::string::npos ||
         text.find("комментар") != std::string::npos ||
         text.find("выберите") != std::string::npos;
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
      out.confidence = clamp01(parsed.value("confidence", out.confidence));
      out.summary = parsed.value("summary", out.summary);
      out.user_action_required = parsed.value("user_action_required", out.user_action_required);
      if (parsed.contains("form_links") && parsed["form_links"].is_array()) {
        out.form_links.clear();
        for (const auto& item : parsed["form_links"]) {
          message_link l;
          l.url = item.value("url", "");
          if (message_has_link(msg, l.url, l)) {
            l.confidence = clamp01(item.value("confidence", l.confidence));
            out.form_links.push_back(l);
          }
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
      json options = json::array();
      for (const auto& option : f.options) {
        options.push_back({{"label", option.label}, {"value", option.value}, {"id", option.id}});
      }
      fields.push_back({
          {"id", f.id},
          {"label", f.label},
          {"normalized_label", f.normalized_label},
          {"type", f.type},
          {"required", f.required},
          {"options", options},
          {"placeholder", f.placeholder},
          {"aria_label", f.aria_label},
          {"question_block_text", f.question_block_text},
          {"nearby_text", f.nearby_text},
          {"validation_error", f.validation_error}
      });
    }

    json profile_json;
    for (const auto& [key, value] : profile.values) {
      if (cfg.privacy_mode == "safe" && is_sensitive_key(key)) {
        continue;
      }
      if (value.empty()) continue;
      profile_json[key] = value;
    }

    std::ostringstream prompt;
    prompt << "Сопоставь поля формы с профилем. Верни только JSON вида "
              "{\"fields\":[{\"field_id\":\"...\",\"semantic_key\":\"full_name|hse_email|personal_email|phone|student_group|faculty|programme|course_year|campus|rating|opinion|consent|unknown|custom.key\","
              "\"mapped_profile_key\":\"...\",\"suggested_value\":\"...\",\"option_value\":\"...\","
              "\"values\":[],\"confidence\":0.0,\"source\":\"llm\",\"requires_user_input\":true,"
              "\"can_auto_fill\":true,\"reason\":\"...\"}]}.\n"
              "field_id must exist. radio/select option_value must match an option label/value/id. "
              "Checkbox values must match options. Consent/privacy/personal-data and opinion/rating/comment fields need explicit user input. "
              "Do not return passwords, tokens, cookies, auth codes or sensitive documents.\n"
           << "Тема письма: " << msg.subject << "\n"
           << "Краткое письмо: " << msg.snippet << "\n"
           << "Тип формы: " << form.form_type << "\n"
           << "Заголовок формы: " << form.title << "\n"
           << "Поля: " << fields.dump() << "\n"
           << "Профиль: " << profile_json.dump();

    json response;
    std::string err;
    auto fallback_fields = fallback->map_fields(msg, form, profile);
    if (!chat(prompt.str(), response, err)) return fallback_fields;

    try {
      std::string content = response.at("message").at("content").get<std::string>();
      json parsed = json::parse(content);
      if (!parsed.contains("fields") || !parsed["fields"].is_array()) {
        std::ostringstream retry;
        retry << prompt.str() << "\n\nYour previous answer was invalid JSON/schema. Return valid JSON with a top-level fields array only.";
        if (!chat(retry.str(), response, err)) return fallback_fields;
        content = response.at("message").at("content").get<std::string>();
        parsed = json::parse(content);
      }
      if (!parsed.contains("fields") || !parsed["fields"].is_array()) return fallback_fields;
      std::map<std::string, form_field> previous_by_id;
      for (const auto& field : fallback_fields) previous_by_id[field.id] = field;
      for (const auto& item : parsed["fields"]) {
        std::string id = item.value("field_id", "");
        for (auto& field : fallback_fields) {
          if (field.id != id) continue;
          if (field.user_modified) continue;
          field.semantic_key = item.value("semantic_key", field.semantic_key);
          if (!semantic_key_allowed(field.semantic_key)) {
            field.semantic_key = "unknown";
            field.requires_user_input = true;
            field.reason = "LLM returned unsupported semantic key";
            field.risk = "high";
            continue;
          }
          field.mapped_profile_key = item.value("mapped_profile_key", field.mapped_profile_key);
          if (cfg.privacy_mode == "safe" && is_sensitive_key(field.mapped_profile_key)) {
            field.requires_user_input = true;
            field.reason = "sensitive profile key blocked in safe mode";
            field.risk = "high";
            continue;
          }
          std::string suggested = item.value("suggested_value", field.value);
          std::string option_value = item.value("option_value", "");
          if (!option_value.empty()) suggested = option_value;
          if (!suggested.empty()) {
            if ((field.type == "radio_group" || field.type == "select") && !option_value_valid(field, suggested)) {
              field.requires_user_input = true;
              field.validation_error = "LLM suggestion does not match available options";
              field.reason = "LLM option suggestion rejected by schema validation";
              field.risk = "high";
            } else {
              field.value = suggested;
              field.suggested_value = suggested;
              field.option_value = option_value;
            }
          }
          if (item.contains("values") && item["values"].is_array()) {
            field.values.clear();
            for (const auto& value : item["values"]) {
              if (value.is_string() && option_value_valid(field, value.get<std::string>())) {
                field.values.push_back(value.get<std::string>());
              }
            }
          }
          field.confidence = clamp01(item.value("confidence", field.confidence));
          field.source = "llm";
          field.reason = item.value("reason", field.reason);
          field.requires_user_input = item.value("requires_user_input", field.requires_user_input);
          field.can_auto_fill = item.value("can_auto_fill", field.can_auto_fill);
          if (field.confidence < 0.75) field.requires_user_input = true;
          if (is_opinion_field(field) && field.value.empty()) field.requires_user_input = true;
        }
      }
      finalize_form_understanding(fallback_fields, profile, previous_by_id, {});
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
        {"format", "json"},
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

bool test_ollama_endpoint(const llm_config& cfg, std::string& err) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }
  json req = {
      {"model", cfg.model},
      {"stream", false},
      {"format", "json"},
      {"messages", json::array({
          {{"role", "system"}, {"content", "Return JSON only."}},
          {{"role", "user"}, {"content", "{\"ping\":true}"}}
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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(std::max(1, cfg.timeout_seconds)));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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
    json response = json::parse(body.empty() ? "{}" : body);
    std::string content = response.at("message").at("content").get<std::string>();
    auto parsed_content = json::parse(content.empty() ? "{}" : content);
    (void)parsed_content;
  } catch (const std::exception& e) {
    err = std::string("ollama JSON-mode probe failed: ") + e.what();
    return false;
  }
  err.clear();
  return true;
}
