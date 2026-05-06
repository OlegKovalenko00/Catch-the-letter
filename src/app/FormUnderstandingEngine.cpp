#include "FormUnderstandingEngine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace {

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const std::vector<std::pair<std::string, std::string>> ru = {
      {"А", "а"}, {"Б", "б"}, {"В", "в"}, {"Г", "г"}, {"Д", "д"}, {"Е", "е"}, {"Ё", "ё"},
      {"Ж", "ж"}, {"З", "з"}, {"И", "и"}, {"Й", "й"}, {"К", "к"}, {"Л", "л"}, {"М", "м"},
      {"Н", "н"}, {"О", "о"}, {"П", "п"}, {"Р", "р"}, {"С", "с"}, {"Т", "т"}, {"У", "у"},
      {"Ф", "ф"}, {"Х", "х"}, {"Ц", "ц"}, {"Ч", "ч"}, {"Ш", "ш"}, {"Щ", "щ"}, {"Ъ", "ъ"},
      {"Ы", "ы"}, {"Ь", "ь"}, {"Э", "э"}, {"Ю", "ю"}, {"Я", "я"}
  };
  for (const auto& [from, to] : ru) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
      text.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return text;
}

bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
  const std::string lower = lower_copy(haystack);
  for (const auto& needle : needles) {
    if (lower.find(lower_copy(needle)) != std::string::npos || haystack.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string profile_value(const user_profile& profile, const std::string& key) {
  auto it = profile.values.find(key);
  if (it == profile.values.end()) return "";
  return it->second;
}

double clamp01(double value) {
  if (value < 0.0) return 0.0;
  if (value > 1.0) return 1.0;
  return value;
}

bool has_value(const form_field& field) {
  return !field.value.empty() || !field.values.empty();
}

bool trueish(const std::string& value) {
  std::string lower = lower_copy(value);
  return lower == "true" || lower == "yes" || lower == "да" || lower == "1" || lower == "on";
}

struct alias_rule {
  std::string semantic_key;
  std::string profile_key;
  std::vector<std::string> aliases;
  double confidence = 0.88;
};

const std::vector<alias_rule>& aliases() {
  static const std::vector<alias_rule> value = {
      {"full_name", "full_name", {"фио", "фамилия имя отчество", "ваше имя", "как вас зовут", "full name"}, 0.92},
      {"last_name", "last_name", {"фамилия", "surname", "last name"}, 0.88},
      {"first_name", "first_name", {"имя", "first name"}, 0.84},
      {"middle_name", "middle_name", {"отчество", "middle name"}, 0.84},
      {"phone", "phone", {"телефон", "номер телефона", "mobile", "phone"}, 0.9},
      {"student_group", "student_group", {"группа", "учебная группа", "номер группы", "student group"}, 0.9},
      {"faculty", "faculty", {"факультет", "faculty"}, 0.88},
      {"programme", "programme", {"образовательная программа", "программа", "направление", "programme", "program"}, 0.86},
      {"course_year", "course_year", {"курс", "course year", "year"}, 0.84},
      {"campus", "campus", {"кампус", "город обучения", "campus"}, 0.86},
      {"education_level", "education_level", {"уровень образования", "education level"}, 0.82}
  };
  return value;
}

std::string choose_email_profile_key(const form_field& field, const user_profile& profile) {
  std::string text = normalized_field_text(field);
  bool hse_context = contains_any(text, {"корпоратив", "почта hse", "edu.hse", "hse email", "учебная почта"});
  if (hse_context && !profile_value(profile, "hse_email").empty()) return "hse_email";
  if (!hse_context && !profile_value(profile, "personal_email").empty()) return "personal_email";
  if (!profile_value(profile, "hse_email").empty()) return "hse_email";
  if (!profile_value(profile, "personal_email").empty()) return "personal_email";
  return hse_context ? "hse_email" : "personal_email";
}

std::string field_label(const form_field& field) {
  if (!field.label.empty()) return field.label;
  if (!field.question_block_text.empty()) return field.question_block_text;
  return field.id;
}

field_validation_issue issue_for(const form_field& field, std::string error) {
  return {field.id, field_label(field), std::move(error)};
}

bool checkbox_group_values_valid(const form_field& field) {
  for (const auto& value : field.values) {
    if (!option_value_valid(field, value)) return false;
  }
  if (field.values.empty() && !field.value.empty()) {
    std::string normalized = field.value;
    for (char& ch : normalized) {
      if (ch == ',' || ch == '[' || ch == ']') ch = ';';
      if (ch == '"' || ch == '\'') ch = ' ';
    }
    std::stringstream ss(normalized);
    std::string part;
    while (std::getline(ss, part, ';')) {
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.erase(part.begin());
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.pop_back();
      if (!part.empty() && !option_value_valid(field, part)) return false;
    }
  }
  return true;
}

}  // namespace

std::string normalized_field_text(const form_field& field) {
  return lower_copy(field.label + " " + field.normalized_label + " " + field.id + " " +
                    field.placeholder + " " + field.aria_label + " " +
                    field.question_block_text + " " + field.nearby_text);
}

bool semantic_key_allowed(const std::string& key) {
  if (key.empty()) return true;
  if (key.rfind("custom.", 0) == 0 && key.size() > 7) return true;
  static const std::set<std::string> allowed = {
      "full_name", "last_name", "first_name", "middle_name", "hse_email", "personal_email",
      "phone", "student_group", "faculty", "programme", "course_year", "campus",
      "education_level", "rating", "opinion", "consent", "unknown"
  };
  return allowed.count(key) > 0;
}

bool option_value_valid(const form_field& field, const std::string& value) {
  if (value.empty()) return true;
  for (const auto& option : field.options) {
    if (option.label == value || option.value == value || option.id == value) return true;
  }
  return false;
}

std::vector<form_field> understand_form_fields_rule_based(const message&,
                                                          const form_snapshot& form,
                                                          const user_profile& profile,
                                                          const form_understanding_options& options) {
  std::vector<form_field> fields = form.fields;
  finalize_form_understanding(fields, profile, {}, options);
  return fields;
}

void finalize_form_understanding(std::vector<form_field>& fields,
                                 const user_profile& profile,
                                 const std::map<std::string, form_field>& previous_by_id,
                                 const form_understanding_options& options) {
  for (auto& field : fields) {
    const auto previous = previous_by_id.find(field.id);
    if (previous != previous_by_id.end() && previous->second.user_modified && options.preserve_user_edits && !options.force) {
      field.value = previous->second.value;
      field.values = previous->second.values;
      field.user_modified = true;
      field.source = "user";
      field.reason = "user override preserved";
      field.mapped_profile_key = previous->second.mapped_profile_key;
      field.semantic_key = previous->second.semantic_key.empty() ? field.semantic_key : previous->second.semantic_key;
      field.confidence = std::max(field.confidence, 1.0);
    }

    field.confidence = clamp01(field.confidence);
    field.validation_error.clear();
    if (field.risk.empty()) field.risk = "low";
    if (field.source.empty()) field.source = "empty";
    if (field.semantic_key.empty()) field.semantic_key = "unknown";
    if (!semantic_key_allowed(field.semantic_key)) {
      field.semantic_key = "unknown";
      field.requires_user_input = true;
      field.reason = "invalid semantic key returned by mapper";
      field.risk = "high";
    }

    const std::string text = normalized_field_text(field);
    const bool is_email = contains_any(text, {"email", "e-mail", "почта", "электронная почта", "edu.hse"});
    const bool is_rating = contains_any(text, {"оценка", "поставьте оценку", "насколько", "rate", "rating"});
    const bool is_opinion = contains_any(text, {"комментар", "отзыв", "мнение", "пожелания", "feedback"});
    const bool is_consent = contains_any(text, {"согласие", "персональные данные", "обработка данных", "privacy", "consent"});

    if (!field.user_modified || options.force) {
      if (is_consent) {
        field.semantic_key = "consent";
        field.source = field.source == "llm" ? field.source : "rule";
        field.reason = "consent/privacy field requires explicit confirmation";
        field.requires_user_input = true;
        field.can_auto_fill = field.user_modified && trueish(field.value);
        field.risk = "high";
      } else if (is_rating) {
        field.semantic_key = "rating";
        field.source = field.source == "llm" ? field.source : "rule";
        field.reason = field.reason.empty() ? "rating field requires explicit user choice" : field.reason;
        field.requires_user_input = !has_value(field);
        field.can_auto_fill = has_value(field);
        field.risk = "medium";
      } else if (is_opinion) {
        field.semantic_key = "opinion";
        field.source = field.source == "llm" ? field.source : "rule";
        field.reason = field.reason.empty() ? "opinion/comment field is left for user input" : field.reason;
        field.requires_user_input = field.required && !has_value(field);
        field.can_auto_fill = false;
        field.risk = "medium";
      } else if (is_email && field.mapped_profile_key.empty()) {
        field.semantic_key = choose_email_profile_key(field, profile) == "hse_email" ? "hse_email" : "personal_email";
        field.mapped_profile_key = choose_email_profile_key(field, profile);
        field.suggested_value = profile_value(profile, field.mapped_profile_key);
        if (!field.suggested_value.empty() && field.value.empty()) field.value = field.suggested_value;
        field.source = "rule";
        field.reason = field.semantic_key == "hse_email" ? "email field has HSE/corporate context" : "generic email field matched profile email";
        field.confidence = std::max(field.confidence, 0.9);
      } else if (field.mapped_profile_key.empty()) {
        for (const auto& alias : aliases()) {
          if (!contains_any(text, alias.aliases)) continue;
          field.semantic_key = alias.semantic_key;
          field.mapped_profile_key = alias.profile_key;
          field.suggested_value = profile_value(profile, alias.profile_key);
          if (!field.suggested_value.empty() && field.value.empty()) field.value = field.suggested_value;
          field.source = "rule";
          field.reason = "matched deterministic semantic alias";
          field.confidence = std::max(field.confidence, alias.confidence);
          break;
        }
      }
    }

    if (field.suggested_value.empty() && !field.value.empty()) field.suggested_value = field.value;
    if ((field.type == "radio_group" || field.type == "select") && !field.value.empty()) {
      if (!option_value_valid(field, field.value)) {
        field.validation_error = "value does not match available options";
        field.requires_user_input = true;
        field.can_auto_fill = false;
        field.risk = "high";
      } else {
        field.option_value = field.value;
      }
    }
    if (field.type == "checkbox_group" && !checkbox_group_values_valid(field)) {
      field.validation_error = "one or more values do not match available options";
      field.requires_user_input = true;
      field.can_auto_fill = false;
      field.risk = "high";
    }

    if (field.confidence < 0.75 && !has_value(field)) field.requires_user_input = field.required;
    if (field.required && !has_value(field)) field.requires_user_input = true;
    if (field.source == "empty" && has_value(field)) field.source = field.user_modified ? "user" : "rule";
  }
}

form_validation_result validate_understood_fields(const std::vector<form_field>& fields) {
  form_validation_result result;
  for (const auto& field : fields) {
    const bool value_present = has_value(field);
    if (field.required && !value_present) {
      result.can_fill = false;
      result.missing_required.push_back(issue_for(field, "required field is empty"));
      continue;
    }
    if (field.required && !field.can_auto_fill) {
      result.can_fill = false;
      result.unsupported_required.push_back(issue_for(
          field, field.unsupported_reason.empty() ? "required field cannot be filled automatically" : field.unsupported_reason));
      continue;
    }
    if ((field.type == "radio_group" || field.type == "select") && value_present && !option_value_valid(field, field.value)) {
      result.can_fill = false;
      result.invalid_options.push_back(issue_for(field, "value does not match available options"));
      continue;
    }
    if (field.type == "checkbox_group" && value_present && !checkbox_group_values_valid(field)) {
      result.can_fill = false;
      result.invalid_options.push_back(issue_for(field, "one or more values do not match available options"));
      continue;
    }
    if (!field.validation_error.empty() && (field.required || value_present)) {
      result.can_fill = false;
      result.invalid_options.push_back(issue_for(field, field.validation_error));
      continue;
    }
    if (!field.required && !value_present && field.requires_user_input) {
      result.warnings.push_back(issue_for(field, "optional field is empty"));
    }
    if (field.risk == "medium" || field.risk == "high") {
      result.warnings.push_back(issue_for(field, field.reason.empty() ? "field requires attention" : field.reason));
    }
  }
  return result;
}

nlohmann::json validation_to_json(const form_validation_result& validation) {
  auto issues_to_json = [](const std::vector<field_validation_issue>& issues) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& issue : issues) {
      arr.push_back({{"field_id", issue.field_id}, {"label", issue.label}, {"error", issue.error}});
    }
    return arr;
  };
  return {
      {"can_fill", validation.can_fill},
      {"missing_required", issues_to_json(validation.missing_required)},
      {"invalid_options", issues_to_json(validation.invalid_options)},
      {"unsupported_required", issues_to_json(validation.unsupported_required)},
      {"warnings", issues_to_json(validation.warnings)}
  };
}

nlohmann::json mapping_summary_to_json(const std::vector<form_field>& fields,
                                       const form_validation_result& validation) {
  int ready = 0;
  int needs_input = 0;
  int unsupported = 0;
  int low_confidence = 0;
  for (const auto& field : fields) {
    if (has_value(field) && field.validation_error.empty()) ready++;
    if (field.requires_user_input) needs_input++;
    if (!field.can_auto_fill) unsupported++;
    if (field.confidence > 0.0 && field.confidence < 0.75) low_confidence++;
  }
  return {
      {"ready", ready},
      {"needs_input", needs_input},
      {"unsupported", unsupported},
      {"low_confidence", low_confidence},
      {"can_fill", validation.can_fill}
  };
}
