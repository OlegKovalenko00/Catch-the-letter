#include "LlmClient.h"

#include "../app/FormUnderstandingEngine.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace {


std::string utf8_lower(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if (c < 0x80) {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      ++i;
    } else if (c == 0xD0 && i + 1 < input.size()) {
      unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
      if (c2 == 0x81) {

        out += '\xD1'; out += '\x91';
      } else if (c2 >= 0x90 && c2 <= 0x9F) {

        out += '\xD0'; out += static_cast<char>(c2 + 0x20);
      } else if (c2 >= 0xA0 && c2 <= 0xAF) {

        out += '\xD1'; out += static_cast<char>(c2 - 0x20);
      } else {
        out += static_cast<char>(c); out += static_cast<char>(c2);
      }
      i += 2;
    } else {

      size_t len = 1;
      if ((c & 0xE0) == 0xC0) len = 2;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xF8) == 0xF0) len = 4;
      for (size_t j = 0; j < len && i + j < input.size(); ++j)
        out += input[i + j];
      i += len;
    }
  }
  return out;
}

bool contains_any(const std::string& text,
                  const std::initializer_list<const char*>& needles) {
  std::string lower = utf8_lower(text);
  for (const char* needle : needles) {
    if (lower.find(needle) != std::string::npos) return true;
  }
  return false;
}

class noop_llm_client final : public llm_client {
public:
  email_analysis analyze_email(const message& msg) override {
    email_analysis result;
    result.summary  = msg.subject.empty() ? msg.snippet : msg.subject;
    result.should_store = true;

    for (const auto& lnk : msg.links) {
      if (lnk.confidence >= 0.8) result.form_links.push_back(lnk);
    }
    result.contains_links       = !msg.links.empty();
    result.contains_attachments = !msg.attachments.empty();

    std::string hay = msg.subject + "\n" + msg.snippet + "\n" + msg.body_text + "\n" + msg.body;


    if (contains_any(hay, {"подтвердите вход", "confirm your login", "two-factor",
                            "2fa", "код подтверждения", "authentication code",
                            "verify your", "подтверждение входа"})) {
      result.kind               = message_kind::auth_required;
      result.level              = importance_level::high;
      result.category           = email_category::security;
      result.urgency            = email_urgency::immediate;
      result.confidence         = 0.85;
      result.importance_score   = 0.85;
      result.user_action_required = true;
      result.should_notify      = true;
      result.reasons.push_back("auth_code_detected");
      return result;
    }


    bool is_confirmation = contains_any(hay, {
        "ответы записаны", "ваш ответ получен", "ответ отправлен",
        "answers recorded", "response submitted", "your response",
        "thank you for", "спасибо за ответ", "/admin/", "/answers/"});


    if (contains_any(hay, {
            "учебн", "задолженн", "отчисл", "пересдач", "приказ",
            "зачёт", "зачет", "экзамен", "оцениван", "задание",
            "уведомлен", "напоминан", "требуется", "документ",
            "lms", "smartlms", "edu", "university", "академическ"})) {
      result.kind               = message_kind::important_notification;
      result.level              = importance_level::high;
      result.category           = email_category::academic;
      result.urgency            = email_urgency::this_week;
      result.confidence         = 0.80;
      result.importance_score   = 0.80;
      result.user_action_required = true;
      result.should_notify      = true;
      result.reasons.push_back("academic_keyword");
      return result;
    }


    if (!is_confirmation) {
      bool has_form_kw = contains_any(hay, {
          "опрос", "анкета", "форма", "регистрация", "заполните",
          "survey", "form", "questionnaire", "fill out", "fill in"});
      if (has_form_kw || !result.form_links.empty()) {
        if (result.form_links.empty() && !msg.links.empty()) {
          auto best = std::max_element(msg.links.begin(), msg.links.end(),
              [](const message_link& a, const message_link& b) {
                return a.confidence < b.confidence; });
          result.form_links.push_back(*best);
        }
        result.kind               = message_kind::form_request;
        result.level              = importance_level::medium;
        result.category           = email_category::form;
        result.urgency            = email_urgency::this_week;
        result.confidence         = result.form_links.empty() ? 0.72 : 0.90;
        result.importance_score   = result.confidence;
        result.contains_form      = true;
        result.user_action_required = true;
        result.should_notify      = true;
        result.reasons.push_back("form_link_or_keyword");
        return result;
      }
    }


    if (contains_any(hay, {
            "важно", "срочно", "дедлайн", "deadline", "important", "urgent",
            "asap", "немедленно", "как можно скорее"})) {
      result.kind               = message_kind::important_notification;
      result.level              = importance_level::medium;
      result.category           = email_category::other;
      result.urgency            = email_urgency::today;
      result.confidence         = 0.65;
      result.importance_score   = 0.65;
      result.should_notify      = true;
      result.reasons.push_back("urgency_keyword");
      return result;
    }

    result.kind             = message_kind::ignored;
    result.level            = importance_level::low;
    result.confidence       = 0.40;
    result.importance_score = 0.20;
    result.should_notify    = false;
    return result;
  }

  std::vector<form_field> map_fields(const message& msg,
                                     const form_snapshot& form,
                                     const user_profile& profile) override {
    return understand_form_fields_rule_based(msg, form, profile);
  }
};

}

std::unique_ptr<llm_client> make_noop_llm_client() {
  return std::make_unique<noop_llm_client>();
}
