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

static std::string json_to_string(const json& value, const std::string& def = "") {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());
  if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
  if (value.is_number_float()) return std::to_string(value.get<double>());
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return def;
}

static std::vector<std::string> json_to_string_vector(const json& value) {
  std::vector<std::string> result;
  if (value.is_array()) {
    for (const auto& item : value) {
      std::string text = json_to_string(item);
      if (!text.empty()) result.push_back(std::move(text));
    }
    return result;
  }

  std::string text = json_to_string(value);
  if (!text.empty()) result.push_back(std::move(text));
  return result;
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
  out.imap.mailbox_id = get_string(imap, "mailbox_id", out.imap.mailbox_id);
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
  out.telegram.poll_updates = get_bool(telegram, "poll_updates", out.telegram.poll_updates);
  out.telegram.poll_interval_seconds =
      get_int(telegram, "poll_interval_seconds", out.telegram.poll_interval_seconds);

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

  const json browser_worker = root.value("browser_worker", json::object());
  out.browser_worker.enabled = get_bool(browser_worker, "enabled", out.browser_worker.enabled);
  out.browser_worker.endpoint = get_string(browser_worker, "endpoint", out.browser_worker.endpoint);
  out.browser_worker.timeout_seconds =
      get_int(browser_worker, "timeout_seconds", out.browser_worker.timeout_seconds);

  const json llm = root.value("llm", json::object());
  out.llm.enabled = get_bool(llm, "enabled", out.llm.enabled);
  out.llm.provider = get_string(llm, "provider", out.llm.provider);
  out.llm.endpoint = get_string(llm, "endpoint", out.llm.endpoint);
  out.llm.model = get_string(llm, "model", out.llm.model);
  out.llm.privacy_mode = get_string(llm, "privacy_mode", out.llm.privacy_mode);
  out.llm.timeout_seconds = get_int(llm, "timeout_seconds", out.llm.timeout_seconds);

  const json security = root.value("security", json::object());
  out.security.mode = get_string(security, "mode", out.security.mode);
  out.security.allow_private_networks =
      get_bool(security, "allow_private_networks", out.security.allow_private_networks);
  out.security.auto_submit = get_bool(security, "auto_submit", out.security.auto_submit);
  out.security.require_confirmation_before_submit = get_bool(
      security,
      "require_confirmation_before_submit",
      out.security.require_confirmation_before_submit
  );
  out.security.allow_password_via_telegram = get_bool(
      security,
      "allow_password_via_telegram",
      out.security.allow_password_via_telegram
  );
  out.security.allowed_domains =
      json_to_string_vector(security.value("allowed_domains", json::array()));
  out.security.blocked_domains =
      json_to_string_vector(security.value("blocked_domains", json::array()));

  const json auth = root.value("auth", json::object());
  out.auth.enabled = get_bool(auth, "enabled", out.auth.enabled);
  out.auth.allow_credentials_via_telegram =
      get_bool(auth, "allow_credentials_via_telegram", out.auth.allow_credentials_via_telegram);
  out.auth.allow_credentials_via_web =
      get_bool(auth, "allow_credentials_via_web", out.auth.allow_credentials_via_web);
  out.auth.remember_credentials = get_bool(auth, "remember_credentials", out.auth.remember_credentials);
  out.auth.credentials_storage = get_string(auth, "credentials_storage", out.auth.credentials_storage);
  out.auth.two_factor_via_telegram =
      get_bool(auth, "two_factor_via_telegram", out.auth.two_factor_via_telegram);
  out.auth.two_factor_via_web =
      get_bool(auth, "two_factor_via_web", out.auth.two_factor_via_web);

  out.profile_file = get_string(root, "profile_file", out.profile_file);
  out.rules_file = get_string(root, "rules_file", out.rules_file);
  out.max_retries = get_int(root, "max_retries", out.max_retries);
  out.backoff_base_ms = get_int(root, "backoff_base_ms", out.backoff_base_ms);
  out.backoff_max_ms = get_int(root, "backoff_max_ms", out.backoff_max_ms);

  const json app = root.value("app", json::object());
  out.events_limit = get_int(app, "events_limit", get_int(root, "events_limit", out.events_limit));
  out.log_level = get_string(app, "log_level", get_string(root, "log_level", out.log_level));
  out.imap.poll_interval_sec = get_int(
      app,
      "poll_interval_seconds",
      out.imap.poll_interval_sec
  );

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
  if (v == "contains_i") {
    if (ok) *ok = true;
    return cond_op::contains_i;
  }
  if (v == "not_contains") {
    if (ok) *ok = true;
    return cond_op::not_contains;
  }
  if (v == "equals") {
    if (ok) *ok = true;
    return cond_op::equals;
  }
  if (v == "not_equals") {
    if (ok) *ok = true;
    return cond_op::not_equals;
  }
  if (v == "regex") {
    if (ok) *ok = true;
    return cond_op::regex;
  }
  if (v == "regex_i") {
    if (ok) *ok = true;
    return cond_op::regex_i;
  }
  if (v == "contains_any") {
    if (ok) *ok = true;
    return cond_op::contains_any;
  }
  if (v == "contains_any_i") {
    if (ok) *ok = true;
    return cond_op::contains_any_i;
  }
  if (v == "exists") {
    if (ok) *ok = true;
    return cond_op::exists;
  }
  if (v == "domain_in") {
    if (ok) *ok = true;
    return cond_op::domain_in;
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
    r.id = json_to_string(rj.value("id", json("")));
    r.name = json_to_string(rj.value("name", json("")));
    if (r.name.empty()) r.name = r.id;
    r.enabled = rj.value("enabled", true);
    r.priority = json_to_string(rj.value("priority", json("info")), "info");

    json conds = rj.value("conditions", json::array());
    if (rj.contains("match") && rj["match"].is_object()) {
      const auto& match_obj = rj["match"];
      r.match = parse_match(json_to_string(match_obj.value("mode", json("all")), "all"));
      conds = match_obj.value("conditions", json::array());
    } else {
      r.match = parse_match(json_to_string(rj.value("match", json("all")), "all"));
    }

    if (conds.is_array()) {
      for (const auto& cj : conds) {
        if (!cj.is_object()) continue;
        condition c;
        c.field = json_to_string(cj.value("field", json("")));
        bool ok = false;
        c.op = parse_op(json_to_string(cj.value("op", json("contains")), "contains"), &ok);
        c.values = json_to_string_vector(cj.value("value", json("")));
        c.value = c.values.empty() ? "" : c.values.front();
        if (c.field.empty() || (!cj.contains("value") && c.op != cond_op::exists) || !ok) continue;
        r.conditions.push_back(c);
      }
    }

    const auto acts = rj.value("actions", json::array());
    if (acts.is_array()) {
      for (const auto& aj : acts) {
        if (!aj.is_object()) continue;
        action a;
        a.type = json_to_string(aj.value("type", json("notify")), "notify");
        a.channel = json_to_string(aj.value("channel", json("telegram")), "telegram");
        a.text = json_to_string(aj.value("text", json("")));
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
      case cond_op::contains_i:
        cj["op"] = "contains_i";
        break;
      case cond_op::not_contains:
        cj["op"] = "not_contains";
        break;
      case cond_op::equals:
        cj["op"] = "equals";
        break;
      case cond_op::not_equals:
        cj["op"] = "not_equals";
        break;
      case cond_op::regex:
        cj["op"] = "regex";
        break;
      case cond_op::regex_i:
        cj["op"] = "regex_i";
        break;
      case cond_op::contains_any:
        cj["op"] = "contains_any";
        break;
      case cond_op::contains_any_i:
        cj["op"] = "contains_any_i";
        break;
      case cond_op::exists:
        cj["op"] = "exists";
        break;
      case cond_op::domain_in:
        cj["op"] = "domain_in";
        break;
    }
    if (c.values.size() > 1) {
      cj["value"] = c.values;
    } else {
      cj["value"] = c.value;
    }
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
