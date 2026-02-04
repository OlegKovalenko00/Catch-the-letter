#include "RuleEngine.h"

#include <regex>
#include <string>
#include <vector>

static std::string field_value(const message& msg, const std::string& field) {
  if (field == "from") return msg.from;
  if (field == "to") return msg.to;
  if (field == "subject") return msg.subject;
  if (field == "snippet") return msg.snippet;
  if (field == "body") return msg.body;
  return "";
}

static bool check_condition(const message& msg, const condition& cond) {
  std::string v = field_value(msg, cond.field);

  if (cond.op == cond_op::contains) {
    return v.find(cond.value) != std::string::npos;
  }

  if (cond.op == cond_op::equals) {
    return v == cond.value;
  }

  if (cond.op == cond_op::regex) {
    try {
      std::regex re(cond.value);
      return std::regex_search(v, re);
    } catch (const std::exception&) {
      return false;
    }
  }

  return false;
}

match_result rule_engine::apply(const message& msg, const std::vector<rule>& rules) const {
  match_result res;
  res.matched = false;

  for (const auto& r : rules) {
    if (!r.enabled) continue;

    bool ok = (r.match == match_mode::all);
    if (r.conditions.empty()) ok = false;

    for (const auto& c : r.conditions) {
      bool matched = check_condition(msg, c);
      if (r.match == match_mode::all && !matched) {
        ok = false;
        break;
      }
      if (r.match == match_mode::any && matched) {
        ok = true;
        break;
      }
    }

    if (ok) {
      res.matched = true;
      res.matched_rule_ids.push_back(r.id);
      for (const auto& a : r.actions) res.actions.push_back(a);
    }
  }

  return res;
}
