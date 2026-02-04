#include "Config.h"

#include "../util/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

using nlohmann::json;

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string expand_env(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    if (input[i] == '$' && i + 1 < input.size() && input[i + 1] == '{') {
      size_t end = input.find('}', i + 2);
      if (end == std::string::npos) {
        out.push_back(input[i]);
        continue;
      }
      std::string key = input.substr(i + 2, end - (i + 2));
      const char* val = std::getenv(key.c_str());
      if (val) out.append(val);
      i = end;
      continue;
    }
    out.push_back(input[i]);
  }
  return out;
}

static std::string get_string(const json& obj, const char* key, const std::string& def = "") {
  if (!obj.contains(key)) return def;
  if (!obj[key].is_string()) return def;
  return expand_env(obj[key].get<std::string>());
}

static int get_int(const json& obj, const char* key, int def) {
  if (!obj.contains(key)) return def;
  if (obj[key].is_number_integer()) return obj[key].get<int>();
  if (obj[key].is_string()) {
    try {
      return std::stoi(obj[key].get<std::string>());
    } catch (...) {
      return def;
    }
  }
  return def;
}

static bool get_bool(const json& obj, const char* key, bool def) {
  if (!obj.contains(key)) return def;
  if (obj[key].is_boolean()) return obj[key].get<bool>();
  if (obj[key].is_number_integer()) return obj[key].get<int>() != 0;
  if (obj[key].is_string()) {
    std::string v = to_lower(obj[key].get<std::string>());
    return v == "1" || v == "true" || v == "yes";
  }
  return def;
}

bool load_app_config(const std::string& path, app_config& out, std::string& err) {
  std::string text;
  if (!json_util::read_file(path, text, &err)) return false;

  json root;
  if (!json_util::parse(text, root, &err)) return false;

  if (!root.is_object()) {
    err = "корневой JSON должен быть объектом";
    return false;
  }

  const json imap = root.value("imap", json::object());
  out.imap.host = get_string(imap, "host");
  out.imap.port = get_int(imap, "port", out.imap.port);
  out.imap.tls = get_bool(imap, "tls", out.imap.tls);
  out.imap.username = get_string(imap, "username");
  out.imap.password = get_string(imap, "password");
  out.imap.folder = get_string(imap, "folder", out.imap.folder);
  out.imap.poll_interval_sec = get_int(imap, "poll_interval_sec", out.imap.poll_interval_sec);
  out.imap.mark_seen = get_bool(imap, "mark_seen", out.imap.mark_seen);

  const json telegram = root.value("telegram", json::object());
  out.telegram.enabled = get_bool(telegram, "enabled", out.telegram.enabled);
  out.telegram.bot_token = get_string(telegram, "bot_token");
  out.telegram.chat_id = get_string(telegram, "chat_id");

  const json twilio = root.value("twilio", json::object());
  out.twilio.enabled = get_bool(twilio, "enabled", out.twilio.enabled);
  out.twilio.account_sid = get_string(twilio, "account_sid");
  out.twilio.auth_token = get_string(twilio, "auth_token");
  out.twilio.from_number = get_string(twilio, "from_number");
  out.twilio.sms_to = get_string(twilio, "sms_to");
  out.twilio.voice_to = get_string(twilio, "voice_to");

  const json http = root.value("http", json::object());
  out.http.enabled = get_bool(http, "enabled", out.http.enabled);
  out.http.host = get_string(http, "host", out.http.host);
  out.http.port = get_int(http, "port", out.http.port);

  const json storage = root.value("storage", json::object());
  out.storage.path = get_string(storage, "path", out.storage.path);

  out.rules_file = get_string(root, "rules_file", out.rules_file);
  out.max_retries = get_int(root, "max_retries", out.max_retries);
  out.backoff_base_ms = get_int(root, "backoff_base_ms", out.backoff_base_ms);
  out.backoff_max_ms = get_int(root, "backoff_max_ms", out.backoff_max_ms);

  if (out.imap.host.empty()) {
    err = "imap.host обязателен";
    return false;
  }
  if (out.imap.username.empty()) {
    err = "imap.username обязателен";
    return false;
  }
  if (out.imap.password.empty()) {
    err = "imap.password обязателен (можно через ${ENV})";
    return false;
  }

  return true;
}

static cond_op parse_op(const std::string& s, bool* ok) {
  std::string v = to_lower(s);
  if (v == "contains") {
    if (ok) *ok = true;
    return cond_op::contains;
  }
  if (v == "equals") {
    if (ok) *ok = true;
    return cond_op::equals;
  }
  if (v == "regex") {
    if (ok) *ok = true;
    return cond_op::regex;
  }
  if (ok) *ok = false;
  return cond_op::contains;
}

static match_mode parse_match(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "any" || v == "or") return match_mode::any;
  return match_mode::all;
}

bool load_rules(const std::string& path, std::vector<rule>& out, std::string& err) {
  std::string text;
  if (!json_util::read_file(path, text, &err)) return false;

  json root;
  if (!json_util::parse(text, root, &err)) return false;

  json rules_json;
  if (root.is_array()) {
    rules_json = root;
  } else if (root.is_object()) {
    rules_json = root.value("rules", json::array());
  } else {
    err = "rules: корневой JSON должен быть массивом или объектом";
    return false;
  }

  if (!rules_json.is_array()) {
    err = "rules: ожидается массив";
    return false;
  }

  std::vector<rule> result;
  for (const auto& rj : rules_json) {
    if (!rj.is_object()) continue;
    rule r;
    r.id = rj.value("id", "");
    r.name = rj.value("name", "");
    r.enabled = rj.value("enabled", true);
    r.priority = rj.value("priority", "info");
    r.match = parse_match(rj.value("match", "all"));

    const auto conds = rj.value("conditions", json::array());
    if (conds.is_array()) {
      for (const auto& cj : conds) {
        if (!cj.is_object()) continue;
        condition c;
        c.field = cj.value("field", "");
        bool ok = false;
        c.op = parse_op(cj.value("op", "contains"), &ok);
        c.value = cj.value("value", "");
        if (c.field.empty() || c.value.empty() || !ok) continue;
        r.conditions.push_back(c);
      }
    }

    const auto acts = rj.value("actions", json::array());
    if (acts.is_array()) {
      for (const auto& aj : acts) {
        if (!aj.is_object()) continue;
        action a;
        a.type = aj.value("type", "notify");
        a.channel = aj.value("channel", "telegram");
        a.text = aj.value("text", "");
        if (a.text.empty()) a.text = r.name;
        r.actions.push_back(a);
      }
    }

    if (!r.id.empty() && !r.name.empty() && !r.conditions.empty()) {
      result.push_back(r);
    }
  }

  out = std::move(result);
  return true;
}

static json rule_to_json(const rule& r) {
  json obj;
  obj["id"] = r.id;
  obj["name"] = r.name;
  obj["enabled"] = r.enabled;
  obj["priority"] = r.priority;
  obj["match"] = (r.match == match_mode::any) ? "any" : "all";

  json conds = json::array();
  for (const auto& c : r.conditions) {
    json cj;
    cj["field"] = c.field;
    switch (c.op) {
      case cond_op::contains:
        cj["op"] = "contains";
        break;
      case cond_op::equals:
        cj["op"] = "equals";
        break;
      case cond_op::regex:
        cj["op"] = "regex";
        break;
    }
    cj["value"] = c.value;
    conds.push_back(cj);
  }
  obj["conditions"] = conds;

  json acts = json::array();
  for (const auto& a : r.actions) {
    json aj;
    aj["type"] = a.type;
    aj["channel"] = a.channel;
    aj["text"] = a.text;
    acts.push_back(aj);
  }
  obj["actions"] = acts;
  return obj;
}

std::string rules_to_json(const std::vector<rule>& rules) {
  json root;
  json arr = json::array();
  for (const auto& r : rules) arr.push_back(rule_to_json(r));
  root["rules"] = arr;
  return root.dump(2);
}
