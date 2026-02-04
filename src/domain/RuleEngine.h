#pragma once

#include "Message.h"
#include "Rule.h"

#include <string>
#include <vector>

struct match_result {
  bool matched;
  std::vector<action> actions;
  std::vector<std::string> matched_rule_ids;
};

class rule_engine {
public:
  match_result apply(const message& msg, const std::vector<rule>& rules) const;
};
