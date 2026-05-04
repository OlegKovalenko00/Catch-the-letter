#pragma once

#include <string>
#include <vector>

enum class cond_op {
  contains,
  contains_i,
  not_contains,
  equals,
  not_equals,
  regex,
  regex_i,
  contains_any,
  contains_any_i,
  exists,
  domain_in,
  date_before,
  date_after
};

enum class match_mode {
  all,
  any
};

struct condition {
  std::string field;
  cond_op op;
  std::string value;
  std::vector<std::string> values;
};

struct action {
  std::string type;
  std::string channel;
  std::string text;
};

struct rule {
  std::string id;
  std::string name;
  bool enabled = true;
  std::string priority = "info";
  match_mode match = match_mode::all;
  std::vector<condition> conditions;
  std::vector<action> actions;
};
