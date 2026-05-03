#include "LlmClient.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains_any(const std::string& text, const std::vector<std::string>& needles) {
  std::string lower = to_lower(text);
  for (const auto& needle : needles) {
    if (lower.find(to_lower(needle)) != std::string::npos) return true;
  }
  return false;
}

std::string profile_value(const user_profile& profile, const std::string& key) {
  auto it = profile.values.find(key);
  if (it != profile.values.end()) return it->second;
  return "";
}

bool field_is_user_input(const std::string& label) {
  return contains_any(label, {
      "opinion", "comment", "rating", "rate", "choose", "select",
      "мнение", "комментар", "оцен", "выберите", "выбор", "отзыв"
  });
}

struct alias_rule {
  std::string key;
  std::vector<std::string> aliases;
};

const std::vector<alias_rule>& aliases() {
  static const std::vector<alias_rule> value = {
      {"full_name", {"фио", "фамилия имя отчество", "full name", "name"}},
      {"hse_email", {"корпоративная почта", "hse email", "edu.hse.ru", "email", "e-mail", "почта"}},
      {"phone", {"телефон", "phone", "mobile", "мобильный"}},
      {"student_group", {"группа", "учебная группа", "student group"}},
      {"faculty", {"факультет", "faculty"}},
      {"programme", {"образовательная программа", "programme", "program", "программа"}},
      {"last_name", {"фамилия", "last name", "surname"}},
      {"first_name", {"имя", "first name"}},
      {"middle_name", {"отчество", "middle name"}},
      {"campus", {"кампус", "campus"}},
      {"education_level", {"уровень образования", "education level"}}
  };
  return value;
}

class noop_llm_client final : public llm_client {
public:
  email_analysis analyze_email(const message& msg) override {
    email_analysis result;
    result.kind = message_kind::unknown;
    result.summary = msg.subject.empty() ? msg.snippet : msg.subject;

    for (const auto& item : msg.links) {
      if (item.confidence >= 0.8) result.form_links.push_back(item);
    }

    std::string haystack = msg.subject + "\n" + msg.snippet + "\n" + msg.body_text + "\n" + msg.body;
    if (!result.form_links.empty() ||
        contains_any(haystack, {"опрос", "анкета", "форма", "регистрация", "заполните", "survey", "form"})) {
      result.kind = message_kind::form_request;
      result.confidence = result.form_links.empty() ? 0.72 : 0.9;
      result.user_action_required = true;
      if (result.form_links.empty() && !msg.links.empty()) {
        auto best = std::max_element(msg.links.begin(), msg.links.end(), [](const link& a, const link& b) {
          return a.confidence < b.confidence;
        });
        result.form_links.push_back(*best);
      }
      return result;
    }

    if (contains_any(haystack, {"важно", "срочно", "дедлайн", "deadline", "important"})) {
      result.kind = message_kind::important_notification;
      result.confidence = 0.8;
      result.user_action_required = true;
      return result;
    }

    result.confidence = 0.4;
    return result;
  }

  std::vector<form_field> map_fields(const message&,
                                     const form_snapshot& form,
                                     const user_profile& profile) override {
    std::vector<form_field> result = form.fields;
    for (auto& field : result) {
      field.confidence = 0.0;
      field.requires_user_input = field.required;

      std::string label = to_lower(field.label + " " + field.id + " " + field.selector);
      if (field_is_user_input(label)) {
        field.requires_user_input = true;
        continue;
      }

      for (const auto& alias : aliases()) {
        bool matched = false;
        for (const auto& needle : alias.aliases) {
          if (label.find(to_lower(needle)) != std::string::npos) {
            matched = true;
            break;
          }
        }
        if (!matched) continue;

        std::string value = profile_value(profile, alias.key);
        if (value.empty()) continue;
        field.mapped_profile_key = alias.key;
        field.value = value;
        field.confidence = 0.86;
        field.requires_user_input = false;
        break;
      }

      if (field.confidence < 0.75) field.requires_user_input = true;
    }
    return result;
  }
};

}  // namespace

std::unique_ptr<llm_client> make_noop_llm_client() {
  return std::make_unique<noop_llm_client>();
}
