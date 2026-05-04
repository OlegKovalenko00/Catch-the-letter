#include "App.h"

#include "../util/Json.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace std::chrono;

static std::string now_iso() {
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32]{};
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

static std::string render_template(std::string text, const message& msg) {
  replace_all(text, "{{from}}", msg.from);
  replace_all(text, "{{to}}", msg.to);
  replace_all(text, "{{subject}}", msg.subject);
  replace_all(text, "{{snippet}}", msg.snippet);
  replace_all(text, "{{date}}", msg.date_iso);
  replace_all(text, "{{uid}}", msg.uid);
  return text;
}

static std::uint64_t parse_uid_or_zero(const std::string& uid) {
  try {
    size_t pos = 0;
    std::uint64_t value = std::stoull(uid, &pos);
    return pos == uid.size() ? value : 0;
  } catch (...) {
    return 0;
  }
}

static std::string lower_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

static nlohmann::json form_field_to_json(const form_field& field) {
  return {
      {"id", field.id},
      {"selector", field.selector},
      {"label", field.label},
      {"type", field.type},
      {"required", field.required},
      {"options", field.options},
      {"value", field.value},
      {"mapped_profile_key", field.mapped_profile_key},
      {"confidence", field.confidence},
      {"requires_user_input", field.requires_user_input}
  };
}

static nlohmann::json form_session_to_json(const form_session& session) {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : session.fields) fields.push_back(form_field_to_json(field));
  return {
      {"id", session.id},
      {"mailbox_id", session.mailbox_id},
      {"message_uid", session.message_uid},
      {"status", session.status},
      {"form_url", session.form_url},
      {"form_type", session.form_type},
      {"title", session.title},
      {"fields", fields},
      {"auth_state", session.auth_state_json},
      {"browser_session_id", session.browser_session_id},
      {"created_at", session.created_at},
      {"updated_at", session.updated_at}
  };
}

app::app(app_config cfg,
         std::unique_ptr<mail_client> mail_client_ptr,
         std::unique_ptr<telegram_notifier> telegram_notifier_ptr,
         std::unique_ptr<storage> storage_ptr,
         std::unique_ptr<twilio_notifier> twilio_ptr)
  : cfg(std::move(cfg)),
    telegram_notifier_ptr(std::move(telegram_notifier_ptr)),
    storage_ptr(std::move(storage_ptr)),
    twilio_ptr(std::move(twilio_ptr)) {
  std::string err;
  if (!load_user_profile(this->cfg.profile_file, profile, err)) {
    profile.values.clear();
  }

  if (mail_client_ptr) {
    mailbox_runtime runtime;
    runtime.cfg = this->cfg.imap;
    runtime.client = std::move(mail_client_ptr);
    mailboxes.push_back(std::move(runtime));
  } else {
    auto mailbox_configs = this->cfg.mailboxes.empty()
        ? std::vector<imap_config>{this->cfg.imap}
        : this->cfg.mailboxes;
    for (const auto& mailbox_cfg : mailbox_configs) {
      mailbox_runtime runtime;
      runtime.cfg = mailbox_cfg;
      if (runtime.cfg.mailbox_id.empty()) runtime.cfg.mailbox_id = "main";

      if (lower_ascii(runtime.cfg.auth_method) == "xoauth2") {
        runtime.last_error = "Gmail XOAUTH2 is not implemented yet";
      } else if (runtime.cfg.host.empty()) {
        runtime.last_error = "imap host is not configured";
      } else if (runtime.cfg.username.empty() || runtime.cfg.password.empty()) {
        runtime.last_error = "imap username/password are not configured";
      } else {
        std::string mail_err;
        runtime.client.reset(make_mail_client_imap(runtime.cfg, &mail_err));
        if (!runtime.client) runtime.last_error = mail_err.empty() ? "imap client unavailable" : mail_err;
      }

      if (!runtime.last_error.empty()) {
        append_event(
            "warn",
            "mailbox_unavailable",
            "Mailbox is not available",
            {{"mailbox_id", runtime.cfg.mailbox_id}, {"provider", runtime.cfg.provider}, {"error", runtime.last_error}}
        );
      }
      mailboxes.push_back(std::move(runtime));
    }
  }

  telegram_bot_ptr = std::make_unique<telegram_bot>(this->cfg.telegram);
  browser_ptr = std::make_unique<browser_worker_client>(this->cfg.browser_worker);
  if (this->cfg.llm.enabled) {
    llm_ptr = make_ollama_client(this->cfg.llm);
  } else {
    llm_ptr = make_noop_llm_client();
  }
  classifier_ptr = std::make_unique<email_classifier>(*llm_ptr);
  workflow_ptr = std::make_unique<workflow_engine>(
      this->cfg,
      *this->storage_ptr,
      engine,
      *classifier_ptr,
      *browser_ptr,
      *llm_ptr,
      profile,
      telegram_bot_ptr.get()
  );
  dialog_manager_ptr = std::make_unique<telegram_dialog_manager>(
      this->cfg.telegram,
      *telegram_bot_ptr,
      *workflow_ptr,
      *this->storage_ptr
  );
}

void app::start_async_services() {
  if (dialog_manager_ptr) dialog_manager_ptr->start();
}

void app::stop_async_services() {
  if (dialog_manager_ptr) dialog_manager_ptr->stop();
}

void app::append_event(std::string level,
                       std::string type,
                       std::string message_text,
                       nlohmann::json data) {
  if (!storage_ptr) return;

  event_record event;
  event.level = std::move(level);
  event.type = std::move(type);
  event.message = std::move(message_text);
  event.data_json = data.dump();
  event.created_at = now_iso();
  storage_ptr->append_event(event, cfg.events_limit);
}

mailbox_checkpoint app::ensure_checkpoint(mailbox_runtime& mailbox) {
  std::string mailbox_id = mailbox.cfg.mailbox_id.empty() ? "main" : mailbox.cfg.mailbox_id;
  auto existing = storage_ptr->load_checkpoint(mailbox_id);
  if (existing.has_value()) {
    std::lock_guard<std::mutex> lock(mu);
    mailbox.last_seen_uid = existing->last_seen_uid;
    status.mailbox_id = existing->mailbox_id;
    status.last_seen_uid = existing->last_seen_uid;
    return existing.value();
  }

  mailbox_checkpoint checkpoint;
  checkpoint.mailbox_id = mailbox_id;
  checkpoint.last_seen_uid = mailbox.client ? mailbox.client->fetch_max_uid() : 0;
  checkpoint.started_at = now_iso();
  checkpoint.updated_at = checkpoint.started_at;
  storage_ptr->save_checkpoint(checkpoint);

  {
    std::lock_guard<std::mutex> lock(mu);
    mailbox.last_seen_uid = checkpoint.last_seen_uid;
    status.mailbox_id = checkpoint.mailbox_id;
    status.last_seen_uid = checkpoint.last_seen_uid;
  }

  append_event(
      "info",
      "checkpoint_initialized",
      "Mailbox checkpoint initialized",
      {{"mailbox_id", checkpoint.mailbox_id}, {"last_seen_uid", checkpoint.last_seen_uid}}
  );
  return checkpoint;
}

void app::load_rules_if_changed() {
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(cfg.rules_file, ec);
  if (ec) return;

  if (rules_mtime == mtime) return;

  std::vector<rule> loaded;
  std::string err;
  if (load_rules(cfg.rules_file, loaded, err)) {
    std::string raw;
    json_util::read_file(cfg.rules_file, raw, nullptr);
    std::lock_guard<std::mutex> lock(mu);
    rules = std::move(loaded);
    rules_raw = std::move(raw);
    rules_mtime = mtime;
  } else {
    std::lock_guard<std::mutex> lock(mu);
    status.last_error = "rules: " + err;
  }
}

bool app::send_action(const message& msg, const action& a, std::string& err) {
  if (a.type != "notify") {
    err = "unknown action type";
    return false;
  }

  std::string text = render_template(a.text, msg);
  std::string channel = a.channel;
  if (channel == "telegram") {
    if (!telegram_notifier_ptr) {
      err = "telegram not configured";
      return false;
    }
    return telegram_notifier_ptr->notify(msg, text, err);
  }
  if (channel == "sms") {
    if (!twilio_ptr) {
      err = "twilio not configured";
      return false;
    }
    return twilio_ptr->send_sms(msg, text, err);
  }
  if (channel == "voice") {
    if (!twilio_ptr) {
      err = "twilio not configured";
      return false;
    }
    return twilio_ptr->make_call(msg, text, err);
  }
  if (channel == "console") {
    std::cout << "[NOTIFY] " << text << std::endl;
    err.clear();
    return true;
  }

  err = "unknown channel";
  return false;
}

void app::run(bool once) {
  if (!storage_ptr) {
    std::cerr << "app not configured" << std::endl;
    return;
  }

  while (true) {
    load_rules_if_changed();
    std::vector<rule> rules_copy;
    {
      std::lock_guard<std::mutex> lock(mu);
      rules_copy = rules;
      status.last_check = now_iso();
    }

    int matched_total = 0;
    for (auto& mailbox : mailboxes) {
      {
        std::lock_guard<std::mutex> lock(mu);
        mailbox.last_check = status.last_check;
      }

      if (!mailbox.client) continue;

      mailbox_checkpoint checkpoint = ensure_checkpoint(mailbox);
      int matched = 0;
      std::uint64_t max_seen = checkpoint.last_seen_uid;
      auto msgs = mailbox.client->fetch_after_uid(checkpoint.last_seen_uid);
      for (auto msg : msgs) {
        if (msg.mailbox_id.empty() || msg.mailbox_id == "default") {
          msg.mailbox_id = checkpoint.mailbox_id;
        }
        if (msg.provider.empty()) msg.provider = mailbox.cfg.provider;

        std::uint64_t numeric_uid = parse_uid_or_zero(msg.uid);
        if (numeric_uid > max_seen) max_seen = numeric_uid;

        if (storage_ptr->is_processed(msg.mailbox_id, msg.uid)) continue;

        auto result = workflow_ptr->handle_message(msg, rules_copy);
        if (result.matched) matched++;
      }

      if (max_seen > checkpoint.last_seen_uid) {
        checkpoint.last_seen_uid = max_seen;
        checkpoint.updated_at = now_iso();
        storage_ptr->save_checkpoint(checkpoint);
        append_event(
            "info",
            "checkpoint_updated",
            "Mailbox checkpoint updated",
            {{"mailbox_id", checkpoint.mailbox_id}, {"last_seen_uid", checkpoint.last_seen_uid}}
        );
      }

      matched_total += matched;
      {
        std::lock_guard<std::mutex> lock(mu);
        mailbox.matched_last = matched;
        mailbox.last_seen_uid = checkpoint.last_seen_uid;
        status.mailbox_id = checkpoint.mailbox_id;
        status.last_seen_uid = checkpoint.last_seen_uid;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      status.processed_total = storage_ptr->processed_count();
      status.matched_last = matched_total;
    }

    if (once) break;
    std::this_thread::sleep_for(seconds(cfg.imap.poll_interval_sec));
  }
}

std::string app::status_json() const {
  std::lock_guard<std::mutex> lock(mu);
  nlohmann::json j;
  j["last_check"] = status.last_check;
  j["last_error"] = status.last_error;
  j["mailbox_id"] = status.mailbox_id;
  j["last_seen_uid"] = status.last_seen_uid;
  j["processed_total"] = status.processed_total;
  j["matched_last"] = status.matched_last;
  j["events_limit"] = cfg.events_limit;
  j["log_level"] = cfg.log_level;
  j["telegram"] = {{"enabled", cfg.telegram.enabled}, {"poll_updates", cfg.telegram.poll_updates}};
  j["browser_worker"] = {{"enabled", cfg.browser_worker.enabled}, {"endpoint", cfg.browser_worker.endpoint}};
  j["llm"] = {{"enabled", cfg.llm.enabled}, {"provider", cfg.llm.provider}, {"model", cfg.llm.model}};
  j["web"] = {{"enabled", cfg.http.enabled}, {"host", cfg.http.host}, {"port", cfg.http.port}};
  j["mailboxes_status"] = nlohmann::json::array();
  for (const auto& mailbox : mailboxes) {
    j["mailboxes_status"].push_back({
        {"id", mailbox.cfg.mailbox_id},
        {"provider", mailbox.cfg.provider},
        {"email", mailbox.cfg.email},
        {"auth_method", mailbox.cfg.auth_method},
        {"folder", mailbox.cfg.folder},
        {"enabled", mailbox.client != nullptr},
        {"last_check", mailbox.last_check},
        {"last_error", mailbox.last_error},
        {"last_seen_uid", mailbox.last_seen_uid},
        {"matched_last", mailbox.matched_last}
    });
  }
  return j.dump(2);
}

std::string app::events_json(int limit) const {
  if (!storage_ptr) return "[]";
  if (limit <= 0) limit = cfg.events_limit;
  if (cfg.events_limit > 0) limit = std::min(limit, cfg.events_limit);

  auto events = storage_ptr->last_events(limit);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& event : events) {
    nlohmann::json item;
    item["id"] = event.id;
    item["level"] = event.level;
    item["type"] = event.type;
    item["message"] = event.message;
    item["created_at"] = event.created_at;

    nlohmann::json data;
    if (json_util::parse(event.data_json.empty() ? "{}" : event.data_json, data, nullptr)) {
      item["data"] = data;
    } else {
      item["data"] = event.data_json;
    }
    out.push_back(std::move(item));
  }
  return out.dump(2);
}

std::string app::rules_json() const {
  std::lock_guard<std::mutex> lock(mu);
  if (!rules_raw.empty()) return rules_raw;
  return rules_to_json(rules);
}

bool app::update_rules_json(const std::string& text, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(text, parsed, &err)) return false;

  std::ofstream f(cfg.rules_file, std::ios::binary);
  if (!f) {
    err = "cannot write rules file";
    return false;
  }
  f << text;
  f.close();
  append_event("info", "rules_updated", "Rules file updated", {{"path", cfg.rules_file}});
  return true;
}

std::string app::active_forms_json(bool all) const {
  auto sessions = storage_ptr->list_active_form_sessions(all);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& session : sessions) out.push_back(form_session_to_json(session));
  return out.dump(2);
}

std::string app::form_json(const std::string& id) const {
  auto session = storage_ptr->get_form_session(id);
  if (!session) return "{}";
  return form_session_to_json(*session).dump(2);
}

bool app::update_form_field_json(const std::string& id, const std::string& body, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(body, parsed, &err)) return false;
  if (parsed.contains("fields") && parsed["fields"].is_array()) {
    int updated = 0;
    for (const auto& item : parsed["fields"]) {
      if (!item.is_object()) continue;
      std::string field_ref = item.value("field_id", item.value("id", item.value("index", "")));
      std::string value = item.value("value", "");
      if (field_ref.empty()) continue;
      if (!workflow_ptr->update_field_value(id, field_ref, value, err)) return false;
      updated++;
    }
    if (updated == 0) {
      err = "fields array does not contain updates";
      return false;
    }
    return true;
  }
  std::string field_ref = parsed.value("field_id", parsed.value("id", parsed.value("index", "")));
  std::string value = parsed.value("value", "");
  if (field_ref.empty()) {
    err = "field_id is required";
    return false;
  }
  return workflow_ptr->update_field_value(id, field_ref, value, err);
}

bool app::fill_form(const std::string& id, std::string& err) {
  return workflow_ptr->fill_form_after_review(id, err);
}

bool app::submit_form(const std::string& id, std::string& err) {
  return workflow_ptr->submit_form_after_confirm(id, err);
}

bool app::mark_form_manual(const std::string& id, std::string& err) {
  return workflow_ptr->mark_manual_required(id, err);
}

bool app::cancel_form(const std::string& id, std::string& err) {
  return workflow_ptr->cancel_form(id, err);
}

bool app::auth_credentials(const std::string& id, const std::string& body, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(body, parsed, &err)) return false;
  if (parsed.value("remember", false)) {
    err = "encrypted credential storage is not implemented";
    return false;
  }
  std::string username = parsed.value("username", parsed.value("login", ""));
  std::string password = parsed.value("password", "");
  if (username.empty() || password.empty()) {
    err = "username and password are required";
    return false;
  }
  return workflow_ptr->submit_auth_credentials(id, username, password, err);
}

bool app::auth_two_factor(const std::string& id, const std::string& body, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(body, parsed, &err)) return false;
  std::string code = parsed.value("code", "");
  if (code.empty()) {
    err = "code is required";
    return false;
  }
  return workflow_ptr->submit_two_factor_code(id, code, err);
}

bool app::reinspect_form(const std::string& id, std::string& err) {
  return workflow_ptr->reinspect_after_auth(id, err);
}

std::string app::profile_json() const {
  return user_profile_to_json(profile);
}

bool app::update_profile_json(const std::string& body, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(body, parsed, &err)) return false;
  std::ofstream f(cfg.profile_file, std::ios::binary);
  if (!f) {
    err = "cannot write profile file";
    return false;
  }
  f << parsed.dump(2);
  f.close();
  load_user_profile(cfg.profile_file, profile, err);
  workflow_ptr->set_profile(profile);
  append_event("info", "profile_updated", "Profile updated", {{"path", cfg.profile_file}});
  return true;
}

std::string app::config_json() const {
  nlohmann::json out;
  out["app"] = {{"events_limit", cfg.events_limit}, {"log_level", cfg.log_level}};
  out["mailboxes"] = nlohmann::json::array();
  auto mailbox_configs = cfg.mailboxes.empty() ? std::vector<imap_config>{cfg.imap} : cfg.mailboxes;
  for (const auto& mailbox : mailbox_configs) {
    out["mailboxes"].push_back({
        {"id", mailbox.mailbox_id},
        {"provider", mailbox.provider},
        {"email", mailbox.email},
        {"auth_method", mailbox.auth_method},
        {"host", mailbox.host},
        {"port", mailbox.port},
        {"tls", mailbox.tls},
        {"folder", mailbox.folder},
        {"checkpoint_mode", mailbox.checkpoint_mode},
        {"poll_interval_sec", mailbox.poll_interval_sec},
        {"mark_seen", mailbox.mark_seen},
        {"username_configured", !mailbox.username.empty()},
        {"password_configured", !mailbox.password.empty()}
    });
  }
  out["imap"] = {
      {"mailbox_id", cfg.imap.mailbox_id},
      {"provider", cfg.imap.provider},
      {"email", cfg.imap.email},
      {"auth_method", cfg.imap.auth_method},
      {"host", cfg.imap.host},
      {"port", cfg.imap.port},
      {"tls", cfg.imap.tls},
      {"folder", cfg.imap.folder},
      {"poll_interval_sec", cfg.imap.poll_interval_sec},
      {"mark_seen", cfg.imap.mark_seen}
  };
  out["telegram"] = {
      {"enabled", cfg.telegram.enabled},
      {"chat_id", cfg.telegram.chat_id.empty() ? "" : "***"},
      {"poll_updates", cfg.telegram.poll_updates},
      {"poll_interval_seconds", cfg.telegram.poll_interval_seconds}
  };
  out["browser_worker"] = {
      {"enabled", cfg.browser_worker.enabled},
      {"endpoint", cfg.browser_worker.endpoint},
      {"timeout_seconds", cfg.browser_worker.timeout_seconds}
  };
  out["llm"] = {
      {"enabled", cfg.llm.enabled},
      {"provider", cfg.llm.provider},
      {"endpoint", cfg.llm.endpoint},
      {"model", cfg.llm.model},
      {"privacy_mode", cfg.llm.privacy_mode}
  };
  out["security"] = {
      {"mode", cfg.security.mode},
      {"allow_private_networks", cfg.security.allow_private_networks},
      {"auto_submit", cfg.security.auto_submit},
      {"require_confirmation_before_submit", cfg.security.require_confirmation_before_submit},
      {"allow_password_via_telegram", cfg.security.allow_password_via_telegram},
      {"allowed_domains", cfg.security.allowed_domains},
      {"blocked_domains", cfg.security.blocked_domains}
  };
  out["auth"] = {
      {"enabled", cfg.auth.enabled},
      {"allow_credentials_via_telegram", cfg.auth.allow_credentials_via_telegram},
      {"allow_credentials_via_web", cfg.auth.allow_credentials_via_web},
      {"remember_credentials", cfg.auth.remember_credentials},
      {"credentials_storage", cfg.auth.credentials_storage},
      {"two_factor_via_telegram", cfg.auth.two_factor_via_telegram},
      {"two_factor_via_web", cfg.auth.two_factor_via_web}
  };
  out["profile_file"] = cfg.profile_file;
  out["rules_file"] = cfg.rules_file;
  return out.dump(2);
}

std::string app::test_browser_json() {
  std::string err;
  bool ok = workflow_ptr->test_browser(err);
  return nlohmann::json({{"ok", ok}, {"error", err}}).dump(2);
}

std::string app::test_llm_json() {
  std::string err;
  bool ok = workflow_ptr->test_llm(err);
  return nlohmann::json({{"ok", ok}, {"error", err}}).dump(2);
}

std::string app::test_telegram_json() {
  std::string err;
  bool ok = workflow_ptr->test_telegram(err);
  return nlohmann::json({{"ok", ok}, {"error", err}}).dump(2);
}

bool app::create_demo_form(bool auth_demo, std::string& err) {
  std::string url = cfg.browser_worker.endpoint + (auth_demo ? "/demo-auth-form" : "/demo-form");
  std::string title = auth_demo ? "Demo auth form" : "Demo form";
  return workflow_ptr->create_demo_session(url, title, auth_demo, err);
}
