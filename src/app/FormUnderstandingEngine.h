#pragma once

#include "../domain/Form.h"
#include "../domain/Message.h"
#include "../domain/UserProfile.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

struct form_understanding_options {
  bool preserve_user_edits = true;
  bool force = false;
};

struct field_validation_issue {
  std::string field_id;
  std::string label;
  std::string error;
};

struct form_validation_result {
  bool can_fill = true;
  std::vector<field_validation_issue> missing_required;
  std::vector<field_validation_issue> invalid_options;
  std::vector<field_validation_issue> unsupported_required;
  std::vector<field_validation_issue> warnings;
};

std::vector<form_field> understand_form_fields_rule_based(const message& msg,
                                                          const form_snapshot& form,
                                                          const user_profile& profile,
                                                          const form_understanding_options& options = {});

void finalize_form_understanding(std::vector<form_field>& fields,
                                 const user_profile& profile,
                                 const std::map<std::string, form_field>& previous_by_id = {},
                                 const form_understanding_options& options = {});

form_validation_result validate_understood_fields(const std::vector<form_field>& fields);
nlohmann::json validation_to_json(const form_validation_result& validation);
nlohmann::json mapping_summary_to_json(const std::vector<form_field>& fields,
                                       const form_validation_result& validation);

bool semantic_key_allowed(const std::string& key);
bool option_value_valid(const form_field& field, const std::string& value);
std::string normalized_field_text(const form_field& field);
