#pragma once

#include <string>
#include <vector>

struct field_option {
  std::string label;
  std::string value;
  std::string selector;
  std::string id;
};

struct form_field {
  std::string id;
  std::string selector;
  std::string label;
  std::string normalized_label;
  std::string type;
  bool required = false;
  std::vector<field_option> options;
  std::string value;
  std::vector<std::string> values;
  std::string semantic_key;
  std::string mapped_profile_key;
  std::string suggested_value;
  std::string option_value;
  double confidence = 0.0;
  std::string source;
  std::string reason;
  std::string risk;
  bool requires_user_input = false;
  bool can_auto_fill = true;
  std::string unsupported_reason;
  bool user_modified = false;
  std::string validation_error;
  std::string question_block_text;
  std::string placeholder;
  std::string aria_label;
  std::string nearby_text;
  std::string yandex_question_id;
  std::vector<std::string> yandex_option_ids;
};

struct form_snapshot {
  std::string session_id;
  std::string url;
  std::string final_url;
  std::string title;
  std::string form_type;
  bool auth_required = false;
  std::string screenshot_path;
  std::string debug_json = "{}";
  std::vector<form_field> fields;
};

struct form_session {
  std::string id;
  std::string mailbox_id;
  std::string message_uid;
  std::string status;
  std::string form_url;
  std::string form_type;
  std::string title;
  std::vector<form_field> fields;
  std::string auth_state_json = "{}";
  std::string browser_session_id;
  std::string created_at;
  std::string updated_at;
};

inline std::vector<std::string> option_labels(const std::vector<field_option>& options) {
  std::vector<std::string> labels;
  for (const auto& option : options) {
    labels.push_back(option.label.empty() ? option.value : option.label);
  }
  return labels;
}

inline bool has_option_label(const std::vector<field_option>& options, const std::string& value) {
  for (const auto& option : options) {
    if (option.label == value || option.value == value || option.id == value) return true;
  }
  return false;
}
