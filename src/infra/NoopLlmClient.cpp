#include "LlmClient.h"

#include "../app/FormUnderstandingEngine.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains_any(const std::string& text, const std::vector<std::string>& needles) {
  std::string lower = to_lower(text);
  for (const auto& needle : needles) {
    if (lower.find(to_lower(needle)) != std::string::npos || text.find(needle) != std::string::npos) return true;
  }
  return false;
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
    // Confirmation/receipt emails should NOT be treated as new form requests.
    bool is_confirmation = contains_any(haystack, {
        "ответы", "answers", "отправлен", "submitted", "получен", "received",
        "ваш ответ", "your response", "thank you for", "спасибо за ответ",
        "/admin/", "/answers/", "response submitted"
    });
    if (!is_confirmation &&
        (!result.form_links.empty() ||
         contains_any(haystack, {"опрос", "анкета", "форма", "регистрация", "заполните", "survey", "form", "questionnaire"}))) {
      result.kind = message_kind::form_request;
      result.confidence = result.form_links.empty() ? 0.72 : 0.9;
      result.user_action_required = true;
      if (result.form_links.empty() && !msg.links.empty()) {
        auto best = std::max_element(msg.links.begin(), msg.links.end(), [](const message_link& a, const message_link& b) {
          return a.confidence < b.confidence;
        });
        result.form_links.push_back(*best);
      }
      return result;
    }

    if (contains_any(haystack, {"важно", "срочно", "дедлайн", "deadline", "important", "exam", "lms", "smartlms"})) {
      result.kind = message_kind::important_notification;
      result.confidence = 0.8;
      result.user_action_required = true;
      return result;
    }

    result.confidence = 0.4;
    return result;
  }

  std::vector<form_field> map_fields(const message& msg,
                                     const form_snapshot& form,
                                     const user_profile& profile) override {
    return understand_form_fields_rule_based(msg, form, profile);
  }
};

}  // namespace

std::unique_ptr<llm_client> make_noop_llm_client() {
  return std::make_unique<noop_llm_client>();
}
