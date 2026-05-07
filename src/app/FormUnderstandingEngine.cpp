#include "FormUnderstandingEngine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

static bool is_blank_text(const std::string& text) {
  // Returns true if text contains only invisible Unicode or whitespace.
  // Yandex Forms uses U+3164 Hangul Filler as placeholder on answer_* inputs.
  // Decodes UTF-8 codepoints and allows only known-invisible ones to pass.
  if (text.empty()) return true;
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) {
      cp = c; len = 1;
    } else if (c < 0xE0 && i + 1 < text.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i+1]) & 0x3Fu); len = 2;
    } else if (c < 0xF0 && i + 2 < text.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i+1]) & 0x3Fu) << 6)
           | (static_cast<unsigned char>(text[i+2]) & 0x3Fu); len = 3;
    } else if (i + 3 < text.size()) {
      cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i+1]) & 0x3Fu) << 12)
           | ((static_cast<unsigned char>(text[i+2]) & 0x3Fu) << 6)
           | (static_cast<unsigned char>(text[i+3]) & 0x3Fu); len = 4;
    }
    i += static_cast<size_t>(len);
    if (cp <= 0x20) continue;                              // ASCII whitespace / control
    if (cp == 0x3164) continue;                            // Hangul Filler (Yandex Forms placeholder)
    if (cp == 0x00A0) continue;                            // NBSP
    if (cp >= 0x200B && cp <= 0x200F) continue;            // ZWSP, ZWNJ, ZWJ, LRM, RLM
    if (cp == 0xFEFF) continue;                            // BOM / ZWNBSP
    if (cp == 0x2060) continue;                            // Word Joiner
    if (cp == 0x180E) continue;                            // Mongolian Vowel Separator
    if (cp == 0x034F) continue;                            // Combining Grapheme Joiner
    return false;  // visible codepoint found
  }
  return true;
}

std::string field_label(const form_field& field) {
  if (!field.label.empty() && !is_blank_text(field.label)) return field.label;
  if (!field.question_block_text.empty() && !is_blank_text(field.question_block_text))
    return field.question_block_text;
  // Yandex Forms: strip "answer_short_text_" prefix to show readable ID for UI display
  if (!field.api_question_id.empty()) {
    const auto& id = field.api_question_id;
    const auto last_under = id.rfind('_');
    if (last_under != std::string::npos) {
      const std::string prefix = id.substr(0, last_under);
      const std::string num = id.substr(last_under + 1);
      if (prefix.rfind("answer_", 0) == 0) return prefix.substr(7) + " #" + num;
    }
  }
  return field.id;
}

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
  // Direct lookup
  auto it = profile.values.find(key);
  if (it != profile.values.end()) return it->second;
  // Cross-lookup: "custom.sex" → "sex" and "sex" → "custom.sex"
  // Needed during migration from old nested {custom:{sex:…}} profile format.
  if (key.rfind("custom.", 0) == 0) {
    auto it2 = profile.values.find(key.substr(7));
    if (it2 != profile.values.end()) return it2->second;
  } else {
    auto it2 = profile.values.find("custom." + key);
    if (it2 != profile.values.end()) return it2->second;
  }
  return "";
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
      {"full_name", "full_name", {"фио", "фамилия имя отчество", "ваше имя", "как вас зовут", "full name",
                                  "ф.и.о", "ф. и. о.", "имя и фамилия", "фамилия и имя"}, 0.92},
      {"last_name", "last_name", {"фамилия", "surname", "last name", "ваша фамилия"}, 0.88},
      {"first_name", "first_name", {"имя", "first name", "ваше имя"}, 0.84},
      {"middle_name", "middle_name", {"отчество", "middle name"}, 0.84},
      {"hse_email", "hse_email", {"почта hse", "hse email", "корпоративная почта", "учебная почта",
                                   "edu.hse", "@edu.hse", "почта студента"}, 0.95},
      {"personal_email", "personal_email", {"email", "e-mail", "почта", "электронная почта",
                                             "почтовый адрес", "electronic mail"}, 0.87},
      {"phone", "phone", {"телефон", "номер телефона", "mobile", "phone", "контактный телефон",
                          "номер для связи"}, 0.9},
      {"student_group", "student_group", {"группа", "учебная группа", "номер группы", "student group",
                                          "академическая группа", "группа студента", "учебная группа студента"}, 0.9},
      {"faculty", "faculty", {"факультет", "faculty", "ваш факультет", "школа", "институт"}, 0.88},
      {"programme", "programme", {"образовательная программа", "программа", "направление", "programme", "program",
                                   "специальность", "направление подготовки"}, 0.86},
      {"course_year", "course_year", {"курс", "course year", "year", "год обучения", "номер курса"}, 0.84},
      {"campus", "campus", {"кампус", "город обучения", "campus", "филиал", "город"}, 0.86},
      {"education_level", "education_level", {"уровень образования", "education level", "бакалавриат",
                                              "магистратура", "аспирантура", "bachelor", "master"}, 0.82},
      {"sex", "sex", {"пол", "ваш пол", "укажите пол", "gender", "sex", "мужской или женский"}, 0.88},
      {"birth_date", "birth_date", {"дата рождения", "день рождения", "год рождения", "дата и год рождения",
                                    "birthday", "birth date", "date of birth", "birthdate"}, 0.88},
      {"nationality", "nationality", {"гражданство", "страна гражданства", "nationality",
                                      "citizenship", "страна проживания"}, 0.84},
      {"department", "department", {"кафедра", "подразделение", "department", "отдел"}, 0.82}
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

static std::string api_answer_type_label_hint(const std::string& api_answer_type) {
  if (api_answer_type == "answer_short_text") return "short text";
  if (api_answer_type == "answer_long_text") return "long text comment opinion";
  if (api_answer_type == "answer_choices") return "choices выберите вариант";
  if (api_answer_type == "answer_date") return "дата date";
  if (api_answer_type == "answer_boolean") return "да нет yes no";
  if (api_answer_type == "answer_number") return "число number";
  return "";
}

std::string normalized_field_text(const form_field& field) {
  std::string extra;
  // If visible text fields are blank, supplement with api_answer_type hint so rules can fire.
  if (!field.api_answer_type.empty() &&
      is_blank_text(field.label) && is_blank_text(field.question_block_text)) {
    extra = " " + api_answer_type_label_hint(field.api_answer_type);
  }
  return lower_copy(field.label + " " + field.normalized_label + " " + field.id + " " +
                    field.placeholder + " " + field.aria_label + " " +
                    field.question_block_text + " " + field.nearby_text + extra);
}

bool semantic_key_allowed(const std::string& key) {
  if (key.empty()) return true;
  if (key.rfind("custom.", 0) == 0 && key.size() > 7) return true;
  static const std::set<std::string> allowed = {
      "full_name", "last_name", "first_name", "middle_name", "hse_email", "personal_email",
      "phone", "student_group", "faculty", "programme", "course_year", "campus",
      "education_level", "sex", "birth_date", "nationality", "department",
      "rating", "opinion", "consent", "unknown"
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
        // Direct profile key pass: for custom keys not covered by aliases, try matching
        // the field label text against the normalized key name (underscores → spaces).
        if (field.mapped_profile_key.empty()) {
          static const std::set<std::string> alias_profile_keys = [] {
            std::set<std::string> s;
            for (const auto& a : aliases()) s.insert(a.profile_key);
            return s;
          }();
          for (const auto& [pkey, pval] : profile.values) {
            if (pval.empty()) continue;
            const std::string bare = (pkey.rfind("custom.", 0) == 0) ? pkey.substr(7) : pkey;
            if (alias_profile_keys.count(bare)) continue;  // already covered by aliases
            std::string normalized = bare;
            std::replace(normalized.begin(), normalized.end(), '_', ' ');
            if (normalized.empty() || !contains_any(text, {normalized, bare})) continue;
            field.mapped_profile_key = bare;
            field.semantic_key = "custom." + bare;
            field.suggested_value = pval;
            if (field.value.empty()) field.value = pval;
            field.source = "rule";
            field.reason = "profile key '" + bare + "' matched field label";
            field.confidence = std::max(field.confidence, 0.65);
            break;
          }
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
