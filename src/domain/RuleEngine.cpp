#include "RuleEngine.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

static bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::vector<std::string> field_values(const message& msg, const std::string& field) {
  if (field == "from") return {msg.from};
  if (field == "to") return {msg.to};
  if (field == "subject") return {msg.subject};
  if (field == "snippet") return {msg.snippet};
  if (field == "body") return {msg.body};
  if (field == "body_text") return {msg.body_text};
  if (field == "body_html") return {msg.body_html};
  if (field == "received_at" || field == "date") return {msg.date_iso};
  if (field == "uid") return {msg.uid};
  if (field == "mailbox_id") return {msg.mailbox_id};
  if (field == "provider") return {msg.provider};
  if (field == "links.url") {
    std::vector<std::string> values;
    for (const auto& item : msg.links) values.push_back(item.url);
    return values;
  }
  if (field == "links.domain") {
    std::vector<std::string> values;
    for (const auto& item : msg.links) values.push_back(item.domain);
    return values;
  }
  if (field == "labels") return msg.labels;
  if (field == "attachments.filename") {
    std::vector<std::string> values;
    for (const auto& item : msg.attachments) values.push_back(item.filename);
    return values;
  }
  if (field == "attachments.mime_type" || field == "attachments.mime") {
    std::vector<std::string> values;
    for (const auto& item : msg.attachments) values.push_back(item.mime_type);
    return values;
  }
  if (field == "attachments.size_bytes") {
    std::vector<std::string> values;
    for (const auto& item : msg.attachments) values.push_back(std::to_string(item.size_bytes));
    return values;
  }
  return {};
}

static bool any_value_contains(const std::vector<std::string>& values, const std::string& needle) {
  for (const auto& value : values) {
    if (value.find(needle) != std::string::npos) return true;
  }
  return false;
}

static bool any_value_equals(const std::vector<std::string>& values, const std::string& expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

static bool any_value_matches_regex(const std::vector<std::string>& values,
                                    const std::string& pattern,
                                    std::regex::flag_type flags) {
  try {
    std::regex re(pattern, flags);
    for (const auto& value : values) {
      if (std::regex_search(value, re)) return true;
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

static bool any_domain_in(const std::vector<std::string>& domains,
                          const std::vector<std::string>& allowed) {
  for (const auto& raw_domain : domains) {
    std::string domain = to_lower(raw_domain);
    for (const auto& raw_allowed : allowed) {
      std::string allowed_domain = to_lower(raw_allowed);
      if (domain == allowed_domain || ends_with(domain, "." + allowed_domain)) {
        return true;
      }
    }
  }
  return false;
}

static bool check_condition(const message& msg, const condition& cond) {
  std::vector<std::string> values = field_values(msg, cond.field);

  if (cond.op == cond_op::contains) {
    return any_value_contains(values, cond.value);
  }

  if (cond.op == cond_op::contains_i) {
    std::vector<std::string> lower_values;
    lower_values.reserve(values.size());
    for (auto value : values) lower_values.push_back(to_lower(std::move(value)));
    return any_value_contains(lower_values, to_lower(cond.value));
  }

  if (cond.op == cond_op::not_contains) {
    return !any_value_contains(values, cond.value);
  }

  if (cond.op == cond_op::equals) {
    return any_value_equals(values, cond.value);
  }

  if (cond.op == cond_op::not_equals) {
    return !any_value_equals(values, cond.value);
  }

  if (cond.op == cond_op::regex) {
    return any_value_matches_regex(values, cond.value, std::regex::ECMAScript);
  }

  if (cond.op == cond_op::regex_i) {
    return any_value_matches_regex(values, cond.value, std::regex::ECMAScript | std::regex::icase);
  }

  if (cond.op == cond_op::contains_any) {
    for (const auto& item : cond.values) {
      if (any_value_contains(values, item)) return true;
    }
    return false;
  }

  if (cond.op == cond_op::contains_any_i) {
    std::vector<std::string> lower_values;
    lower_values.reserve(values.size());
    for (auto value : values) lower_values.push_back(to_lower(std::move(value)));
    for (const auto& item : cond.values) {
      if (any_value_contains(lower_values, to_lower(item))) return true;
    }
    return false;
  }

  if (cond.op == cond_op::exists) {
    return std::any_of(values.begin(), values.end(), [](const std::string& value) {
      return !value.empty();
    });
  }

  if (cond.op == cond_op::domain_in) {
    return any_domain_in(values, cond.values);
  }

  if (cond.op == cond_op::date_before) {
    for (const auto& value : values) {
      if (!value.empty() && value < cond.value) return true;
    }
    return false;
  }

  if (cond.op == cond_op::date_after) {
    for (const auto& value : values) {
      if (!value.empty() && value > cond.value) return true;
    }
    return false;
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
