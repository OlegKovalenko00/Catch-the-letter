#pragma once

#include "Message.h"

#include <string>
#include <vector>

enum class message_kind {
  ignored,
  important_notification,
  action_required,
  form_request,
  auth_required,
  unknown
};

enum class importance_level {
  critical,
  high,
  medium,
  low,
  ignore
};

enum class email_category {
  academic,
  admin,
  finance,
  security,
  form,
  schedule,
  document,
  spam,
  other
};

enum class email_urgency {
  immediate,
  today,
  this_week,
  no_deadline,
  unknown
};

struct email_analysis {
  message_kind kind = message_kind::unknown;
  double confidence = 0.0;
  double importance_score = 0.0;
  importance_level level = importance_level::low;
  email_category category = email_category::other;
  email_urgency urgency = email_urgency::unknown;
  std::string summary;
  std::string safe_preview;
  std::vector<std::string> reasons;
  std::vector<std::string> suggested_actions;
  std::vector<message_link> form_links;
  bool user_action_required = false;
  bool should_notify = false;
  bool should_store = true;
  bool contains_form = false;
  bool contains_links = false;
  bool contains_attachments = false;
  std::string attachment_relevance;
  std::string deadline_text;
  std::string deadline_iso;
};

inline const char* to_string(message_kind k) {
  switch (k) {
    case message_kind::ignored:               return "ignored";
    case message_kind::important_notification: return "important_notification";
    case message_kind::action_required:        return "action_required";
    case message_kind::form_request:           return "form_request";
    case message_kind::auth_required:          return "auth_required";
    case message_kind::unknown:                return "unknown";
  }
  return "unknown";
}

inline const char* to_string(importance_level l) {
  switch (l) {
    case importance_level::critical: return "critical";
    case importance_level::high:     return "high";
    case importance_level::medium:   return "medium";
    case importance_level::low:      return "low";
    case importance_level::ignore:   return "ignore";
  }
  return "low";
}

inline const char* to_string(email_category c) {
  switch (c) {
    case email_category::academic:  return "academic";
    case email_category::admin:     return "admin";
    case email_category::finance:   return "finance";
    case email_category::security:  return "security";
    case email_category::form:      return "form";
    case email_category::schedule:  return "schedule";
    case email_category::document:  return "document";
    case email_category::spam:      return "spam";
    case email_category::other:     return "other";
  }
  return "other";
}

inline const char* to_string(email_urgency u) {
  switch (u) {
    case email_urgency::immediate:   return "immediate";
    case email_urgency::today:       return "today";
    case email_urgency::this_week:   return "this_week";
    case email_urgency::no_deadline: return "no_deadline";
    case email_urgency::unknown:     return "unknown";
  }
  return "unknown";
}

inline message_kind parse_message_kind(const std::string& s) {
  if (s == "important_notification") return message_kind::important_notification;
  if (s == "action_required")        return message_kind::action_required;
  if (s == "form_request")           return message_kind::form_request;
  if (s == "auth_required")          return message_kind::auth_required;
  if (s == "ignored")                return message_kind::ignored;
  return message_kind::unknown;
}

inline importance_level parse_importance_level(const std::string& s) {
  if (s == "critical") return importance_level::critical;
  if (s == "high")     return importance_level::high;
  if (s == "medium")   return importance_level::medium;
  if (s == "low")      return importance_level::low;
  if (s == "ignore")   return importance_level::ignore;
  return importance_level::low;
}

inline email_category parse_email_category(const std::string& s) {
  if (s == "academic")  return email_category::academic;
  if (s == "admin")     return email_category::admin;
  if (s == "finance")   return email_category::finance;
  if (s == "security")  return email_category::security;
  if (s == "form")      return email_category::form;
  if (s == "schedule")  return email_category::schedule;
  if (s == "document")  return email_category::document;
  if (s == "spam")      return email_category::spam;
  return email_category::other;
}

inline email_urgency parse_email_urgency(const std::string& s) {
  if (s == "immediate")   return email_urgency::immediate;
  if (s == "today")       return email_urgency::today;
  if (s == "this_week")   return email_urgency::this_week;
  if (s == "no_deadline") return email_urgency::no_deadline;
  return email_urgency::unknown;
}
