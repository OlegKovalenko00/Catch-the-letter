#pragma once

#include <string>
#include <vector>

enum class cond_op {
  contains,
  equals,
  regex
};

enum class match_mode {
  all,
  any
};

struct condition {
  std::string field;
  cond_op op;
  std::string value;
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
