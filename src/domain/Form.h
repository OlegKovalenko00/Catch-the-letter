#pragma once

#include <string>
#include <vector>

struct form_field {
  std::string id;
  std::string selector;
  std::string label;
  std::string type;
  bool required = false;
  std::vector<std::string> options;
  std::string value;
  std::string mapped_profile_key;
  double confidence = 0.0;
  bool requires_user_input = false;
};

struct form_snapshot {
  std::string session_id;
  std::string url;
  std::string final_url;
  std::string title;
  std::string form_type;
  bool auth_required = false;
  std::string screenshot_path;
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
