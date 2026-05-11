#include "ProfileExpansionService.h"
#include "ProfileFactGraph.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using nlohmann::json;

namespace {

const std::unordered_set<std::string>& allowed_expansion_keys() {
  static const std::unordered_set<std::string> keys{
    "first_name", "last_name", "middle_name", "full_name",
    "first_last", "last_first", "initials",
    "sex_ru", "sex_en", "sex_short_ru", "sex_canonical",
    "birth_date", "nationality",
    "email", "primary_email", "corporate_email", "edu_email",
    "student_group", "group", "academic_group",
    "faculty", "programme", "course_year", "campus", "education_level",
    "student_id_number", "record_book_number", "department", "role",
    "telegram_username"
  };
  return keys;
}

static const std::vector<std::string> k_sensitive_fragments = {
  "password", "token", "secret", "cookie", "passport", "snils"
};

std::string get_profile_fact(const user_profile& profile, const std::string& key) {
  auto it = profile.values.find(key);
  if (it != profile.values.end() && !it->second.empty()) return it->second;
  auto it2 = profile.values.find("custom." + key);
  if (it2 != profile.values.end() && !it2->second.empty()) return it2->second;
  return "";
}

bool is_sensitive(const std::string& key) {
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& frag : k_sensitive_fragments) {
    if (lower.find(frag) != std::string::npos) return true;
  }
  return false;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string rule_reason(const std::string& key) {
  if (key == "first_name" || key == "last_name" || key == "middle_name")
    return "Выведено из full_name";
  if (key == "initials")
    return "Инициалы из full_name";
  if (key == "full_name" || key == "first_last" || key == "last_first")
    return "Составлено из компонентов имени";
  if (key == "sex_ru" || key == "sex_en" || key == "sex_short_ru" || key == "sex_canonical")
    return "Вариант поля sex";
  if (key == "email" || key == "primary_email")
    return "Псевдоним personal_email";
  if (key == "corporate_email" || key == "edu_email")
    return "Псевдоним hse_email";
  if (key == "group" || key == "academic_group")
    return "Псевдоним student_group";
  return "Производное поле";
}

}

bool is_expansion_key_allowed(const std::string& key) {
  return allowed_expansion_keys().count(key) > 0;
}

std::vector<expansion_suggestion> suggest_profile_rules(const user_profile& profile) {
  user_profile expanded = profile;
  expand_profile_facts(expanded);

  std::vector<expansion_suggestion> result;
  for (const auto& [key, value] : expanded.values) {
    if (value.empty()) continue;
    if (!is_expansion_key_allowed(key)) continue;
    if (!get_profile_fact(profile, key).empty()) continue;
    result.push_back({key, value, "rule", rule_reason(key), 0.95});
  }
  return result;
}

std::vector<expansion_suggestion> suggest_profile_llm(const user_profile& profile,
                                                       const llm_config& cfg) {
  if (!cfg.enabled || cfg.endpoint.empty() || cfg.model.empty()) return {};

  std::ostringstream profile_text;
  for (const auto& [key, value] : profile.values) {
    if (value.empty()) continue;
    if (is_sensitive(key)) continue;
    profile_text << key << ": " << value << "\n";
  }

  std::ostringstream allowed_list;
  for (const auto& k : allowed_expansion_keys()) allowed_list << k << ", ";

  const std::string system_prompt =
    "Ты помощник по заполнению профиля студента. "
    "Анализируй только предоставленные данные. "
    "Не придумывай данные, которых нет в профиле. "
    "Верни ТОЛЬКО JSON-массив без пояснений.";

  std::ostringstream user_prompt;
  user_prompt
    << "Текущий профиль:\n" << profile_text.str() << "\n"
    << "Предложи ТОЛЬКО отсутствующие поля, которые точно выводятся из данных выше.\n"
    << "Разрешённые ключи: " << allowed_list.str() << "\n"
    << "НЕ предлагай поля уже присутствующие в профиле выше.\n"
    << "НЕ предлагай пароли, токены, паспорт, СНИЛС.\n"
    << "confidence от 0.0 до 1.0. Включай только поля с confidence > 0.7.\n"
    << "Формат: [{\"key\":\"...\",\"value\":\"...\",\"reason\":\"...\",\"confidence\":0.0}]\n"
    << "Верни [] если нет явных выводов с confidence > 0.7.";

  json req = {
    {"model", cfg.model},
    {"stream", false},
    {"format", "json"},
    {"think", false},
    {"options", {{"temperature", 0}, {"num_predict", 512}}},
    {"messages", json::array({
      {{"role", "system"}, {"content", system_prompt}},
      {{"role", "user"}, {"content", user_prompt.str()}}
    })}
  };

  std::string payload = req.dump();
  std::string body;
  CURL* curl = curl_easy_init();
  if (!curl) return {};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, cfg.endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(std::max(1, cfg.timeout_seconds)));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || code < 200 || code >= 300) return {};

  try {
    json response = json::parse(body.empty() ? "{}" : body);
    std::string content = response.at("message").at("content").get<std::string>();
    json parsed = json::parse(content);
    if (!parsed.is_array()) return {};

    std::vector<expansion_suggestion> result;
    for (const auto& item : parsed) {
      if (!item.is_object()) continue;
      std::string key    = item.value("key", "");
      std::string value  = item.value("value", "");
      std::string reason = item.value("reason", "Предложено LLM");
      double confidence  = item.value("confidence", 0.0);
      if (key.empty() || value.empty()) continue;
      if (!is_expansion_key_allowed(key)) continue;
      if (is_sensitive(key)) continue;
      if (confidence < 0.7) continue;
      if (!get_profile_fact(profile, key).empty()) continue;
      result.push_back({key, value, "llm", reason, confidence});
    }
    return result;
  } catch (...) {
    return {};
  }
}

std::vector<expansion_suggestion> suggest_profile_expansions(const user_profile& profile,
                                                              const llm_config& cfg,
                                                              bool use_llm) {
  auto suggestions = suggest_profile_rules(profile);

  if (use_llm) {
    std::unordered_set<std::string> seen;
    for (const auto& s : suggestions) seen.insert(s.key);

    for (auto& s : suggest_profile_llm(profile, cfg)) {
      if (!seen.count(s.key)) {
        seen.insert(s.key);
        suggestions.push_back(std::move(s));
      }
    }
  }

  return suggestions;
}
