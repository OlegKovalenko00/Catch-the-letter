#include "App.h"

#include "FormUnderstandingEngine.h"

#include "../util/Json.h"
#include "../infra/UrlPolicy.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
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
  nlohmann::json options = nlohmann::json::array();
  for (const auto& option : field.options) {
    options.push_back({
        {"label", option.label},
        {"value", option.value},
        {"selector", option.selector},
        {"id", option.id}
    });
  }
  return {
      {"id", field.id},
      {"selector", field.selector},
      {"label", field.label},
      {"normalized_label", field.normalized_label},
      {"type", field.type},
      {"required", field.required},
      {"options", options},
      {"value", field.value},
      {"values", field.values},
      {"semantic_key", field.semantic_key},
      {"mapped_profile_key", field.mapped_profile_key},
      {"suggested_value", field.suggested_value},
      {"option_value", field.option_value},
      {"confidence", field.confidence},
      {"source", field.source},
      {"reason", field.reason},
      {"risk", field.risk},
      {"requires_user_input", field.requires_user_input},
      {"can_auto_fill", field.can_auto_fill},
      {"unsupported_reason", field.unsupported_reason},
      {"user_modified", field.user_modified},
      {"validation_error", field.validation_error},
      {"question_block_text", field.question_block_text},
      {"placeholder", field.placeholder},
      {"aria_label", field.aria_label},
      {"nearby_text", field.nearby_text},
      {"yandex_question_id", field.yandex_question_id},
      {"yandex_option_ids", field.yandex_option_ids}
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

static nlohmann::json form_snapshot_to_json(const form_snapshot& snapshot) {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : snapshot.fields) fields.push_back(form_field_to_json(field));
  nlohmann::json debug = nlohmann::json::object();
  try {
    debug = nlohmann::json::parse(snapshot.debug_json.empty() ? "{}" : snapshot.debug_json);
  } catch (...) {
    debug = snapshot.debug_json;
  }
  return {
      {"session_id", snapshot.session_id},
      {"url", snapshot.url},
      {"final_url", snapshot.final_url},
      {"title", snapshot.title},
      {"form_type", snapshot.form_type},
      {"auth_required", snapshot.auth_required},
      {"screenshot_path", snapshot.screenshot_path},
      {"fields", fields},
      {"debug", debug}
  };
}

static std::string message_kind_name(message_kind kind) {
  switch (kind) {
    case message_kind::ignored: return "ignored";
    case message_kind::important_notification: return "important_notification";
    case message_kind::form_request: return "form_request";
    case message_kind::auth_required: return "auth_required";
    case message_kind::unknown: return "unknown";
  }
  return "unknown";
}

static std::string redact_proxy_url(const std::string& url) {
  auto scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  size_t userinfo_start = scheme + 3;
  size_t at = url.find('@', userinfo_start);
  if (at == std::string::npos) return url;
  size_t colon = url.find(':', userinfo_start);
  if (colon == std::string::npos || colon > at) {
    return url.substr(0, userinfo_start) + "<redacted>@" + url.substr(at + 1);
  }
  return url.substr(0, colon + 1) + "<redacted>@" + url.substr(at + 1);
}

static nlohmann::json api_success(std::string message, nlohmann::json data) {
  nlohmann::json out = data;
  out["ok"] = true;
  out["message"] = std::move(message);
  out["data"] = std::move(data);
  return out;
}

static nlohmann::json api_error(std::string error, nlohmann::json details = nlohmann::json::object()) {
  return {
      {"ok", false},
      {"error", std::move(error)},
      {"details", std::move(details)}
  };
}

static message make_sample_form_message() {
  message msg;
  msg.uid = "test-llm";
  msg.mailbox_id = "test";
  msg.provider = "demo";
  msg.from = "teacher@hse.ru";
  msg.subject = "Заполните форму обратной связи";
  msg.snippet = "Нужно заполнить анкету";
  msg.body_text = "Пожалуйста, заполните форму: https://forms.yandex.ru/u/test/";
  msg.links.push_back({"https://forms.yandex.ru/u/test/", "forms.yandex.ru", 0.95});
  return msg;
}

static user_profile make_sample_profile() {
  user_profile sample;
  sample.values["full_name"] = "Иванов Иван Иванович";
  sample.values["hse_email"] = "student@edu.hse.ru";
  sample.values["personal_email"] = "student@example.com";
  sample.values["student_group"] = "БПИ000";
  return sample;
}

static form_snapshot make_sample_mapping_form() {
  form_snapshot snapshot;
  snapshot.url = "https://forms.yandex.ru/u/test/";
  snapshot.form_type = "yandex_forms";
  snapshot.title = "Тестовая форма";
  form_field full_name;
  full_name.id = "full_name";
  full_name.label = "ФИО";
  full_name.type = "text";
  full_name.required = true;
  snapshot.fields.push_back(full_name);

  form_field email;
  email.id = "email";
  email.label = "Email";
  email.type = "email";
  email.required = true;
  snapshot.fields.push_back(email);

  form_field group;
  group.id = "group";
  group.label = "Группа";
  group.type = "text";
  group.required = true;
  snapshot.fields.push_back(group);

  form_field rating;
  rating.id = "rating";
  rating.label = "Оценка";
  rating.type = "radio_group";
  rating.required = true;
  rating.options = {{"5", "5", "", ""}, {"4", "4", "", ""}, {"3", "3", "", ""}};
  snapshot.fields.push_back(rating);
  return snapshot;
}

static bool field_has_label(const form_field& field, const std::string& text) {
  return field.label.find(text) != std::string::npos ||
         field.normalized_label.find(text) != std::string::npos ||
         field.id.find(text) != std::string::npos;
}

static bool has_demo_rating_radio_group(const std::vector<form_field>& fields) {
  for (const auto& field : fields) {
    if (field.type != "radio_group") continue;
    if (!field_has_label(field, "Оценка") && !field_has_label(field, "rating")) continue;
    if (has_option_label(field.options, "5") &&
        has_option_label(field.options, "4") &&
        has_option_label(field.options, "3")) {
      return true;
    }
  }
  return false;
}

static bool has_checkbox_group(const std::vector<form_field>& fields) {
  for (const auto& field : fields) {
    if (field.type == "checkbox_group" && !field.options.empty()) return true;
  }
  return false;
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
  j["telegram"] = {
      {"enabled", cfg.telegram.enabled},
      {"poll_updates", cfg.telegram.poll_updates},
      {"token_configured", !cfg.telegram.bot_token.empty()},
      {"chat_id_configured", !cfg.telegram.chat_id.empty()},
      {"proxy_configured", !cfg.telegram.proxy_url.empty()},
      {"proxy_url_redacted", redact_proxy_url(cfg.telegram.proxy_url)}
  };
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
      {"bot_token_configured", !cfg.telegram.bot_token.empty()},
      {"proxy_configured", !cfg.telegram.proxy_url.empty()},
      {"proxy_url_redacted", redact_proxy_url(cfg.telegram.proxy_url)},
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
  nlohmann::json out = {
      {"ok", false},
      {"worker_reachable", false},
      {"demo_inspect_ok", false},
      {"radio_group_ok", false},
      {"checkbox_group_ok", false},
      {"endpoint", cfg.browser_worker.endpoint},
      {"error", ""}
  };
  if (!browser_ptr || !browser_ptr->health(err)) {
    out["error"] = err.empty() ? "browser-worker unavailable" : err;
    return out.dump(2);
  }
  out["worker_reachable"] = true;

  std::string url = cfg.browser_worker.endpoint + "/demo-form";
  auto snapshot = browser_ptr->inspect_form(url, err, true);
  if (!snapshot.has_value()) {
    out["error"] = err.empty() ? "demo form inspect failed" : err;
    return out.dump(2);
  }
  out["demo_inspect_ok"] = true;
  out["form_type"] = snapshot->form_type;
  out["field_count"] = snapshot->fields.size();
  out["radio_group_ok"] = has_demo_rating_radio_group(snapshot->fields);
  out["checkbox_group_ok"] = has_checkbox_group(snapshot->fields);
  out["debug"] = form_snapshot_to_json(*snapshot)["debug"];
  if (!snapshot->session_id.empty()) {
    std::string close_err;
    browser_ptr->close_session(snapshot->session_id, close_err);
  }
  out["ok"] = out["worker_reachable"].get<bool>() &&
              out["demo_inspect_ok"].get<bool>() &&
              out["radio_group_ok"].get<bool>() &&
              out["checkbox_group_ok"].get<bool>();
  if (!out["ok"].get<bool>()) out["error"] = "demo form fields were not extracted as expected";
  return out.dump(2);
}

std::string app::test_imap_json() {
  nlohmann::json out;
  out["ok"] = true;
  out["mailboxes"] = nlohmann::json::array();
  auto mailbox_configs = cfg.mailboxes.empty() ? std::vector<imap_config>{cfg.imap} : cfg.mailboxes;
  for (const auto& mailbox : mailbox_configs) {
    nlohmann::json item = {
        {"id", mailbox.mailbox_id},
        {"provider", mailbox.provider},
        {"reachable", false},
        {"auth_ok", false},
        {"folder_ok", false},
        {"max_uid", 0},
        {"skipped", false},
        {"error", ""}
    };
    if (lower_ascii(mailbox.auth_method) == "xoauth2") {
      item["skipped"] = true;
      item["error"] = "Gmail XOAUTH2 is not implemented yet";
      out["ok"] = false;
      out["mailboxes"].push_back(std::move(item));
      continue;
    }
    if (mailbox.username.empty() || mailbox.password.empty()) {
      item["skipped"] = true;
      item["error"] = "imap username/password are not configured";
      out["ok"] = false;
      out["mailboxes"].push_back(std::move(item));
      continue;
    }
    auto result = test_imap_mailbox(mailbox);
    item["reachable"] = result.reachable;
    item["auth_ok"] = result.auth_ok;
    item["folder_ok"] = result.folder_ok;
    item["max_uid"] = result.max_uid;
    item["error"] = result.error;
    if (!result.reachable || !result.auth_ok || !result.folder_ok) out["ok"] = false;
    out["mailboxes"].push_back(std::move(item));
  }
  return out.dump(2);
}

std::string app::test_llm_json() {
  std::string err;
  bool reachable = false;
  bool fallback = false;
  std::unique_ptr<llm_client> test_client;
  std::string active_client = "NoopLlmClient";

  if (cfg.llm.enabled) {
    reachable = test_ollama_endpoint(cfg.llm, err);
    if (reachable) {
      test_client = make_ollama_client(cfg.llm);
      active_client = "OllamaClient";
    } else {
      fallback = true;
      append_event("warn", "llm_fallback", "Ollama unavailable, using NoopLlmClient", {{"error", err}});
      test_client = make_noop_llm_client();
    }
  } else {
    test_client = make_noop_llm_client();
  }

  message msg = make_sample_form_message();
  auto analysis = test_client->analyze_email(msg);
  form_snapshot sample_form = make_sample_mapping_form();
  auto mapped = test_client->map_fields(msg, sample_form, make_sample_profile());

  auto mapping_ok = [](const std::vector<form_field>& fields) {
    bool full_name = false;
    bool email = false;
    bool group = false;
    for (const auto& field : fields) {
      if (field.label == "ФИО" && field.mapped_profile_key == "full_name" && !field.value.empty()) full_name = true;
      if (field.label == "Email" &&
          (field.mapped_profile_key == "hse_email" || field.mapped_profile_key == "personal_email") &&
          !field.value.empty()) {
        email = true;
      }
      if (field.label == "Группа" && field.mapped_profile_key == "student_group" && !field.value.empty()) group = true;
    }
    return full_name && email && group;
  };

  bool sample_mapping_ok = mapping_ok(mapped);
  if (cfg.llm.enabled && reachable && !sample_mapping_ok) {
    fallback = true;
    active_client = "NoopLlmClient";
    auto noop = make_noop_llm_client();
    mapped = noop->map_fields(msg, sample_form, make_sample_profile());
    sample_mapping_ok = mapping_ok(mapped);
    append_event("warn", "llm_fallback", "Ollama sample mapping failed validation, using NoopLlmClient");
  }

  nlohmann::json mapped_fields = nlohmann::json::object();
  int mapped_count = 0;
  int needs_input_count = 0;
  for (const auto& field : mapped) {
    if (!field.mapped_profile_key.empty() || !field.value.empty()) mapped_count++;
    if (field.requires_user_input) needs_input_count++;
    mapped_fields[field.label.empty() ? field.id : field.label] = {
        {"mapped_profile_key", field.mapped_profile_key},
        {"semantic_key", field.semantic_key},
        {"value", field.value},
        {"suggested_value", field.suggested_value},
        {"confidence", field.confidence},
        {"source", field.source},
        {"reason", field.reason},
        {"risk", field.risk}
    };
  }

  nlohmann::json out = {
      {"ok", sample_mapping_ok && analysis.kind == message_kind::form_request},
      {"enabled", cfg.llm.enabled},
      {"provider", cfg.llm.enabled ? cfg.llm.provider : "noop"},
      {"active_client", active_client},
      {"model", cfg.llm.model},
      {"endpoint", cfg.llm.endpoint},
      {"reachable", reachable},
      {"fallback", fallback},
      {"sample_classification", message_kind_name(analysis.kind)},
      {"sample_mapping_ok", sample_mapping_ok},
      {"mapped_count", mapped_count},
      {"needs_input_count", needs_input_count},
      {"invalid_json_count", 0},
      {"invalid_schema_count", 0},
      {"last_llm_error", (cfg.llm.enabled && !reachable) ? err : ""},
      {"fallback_used", fallback},
      {"mapped_fields", mapped_fields},
      {"error", (cfg.llm.enabled && !reachable) ? err : ""}
  };
  return out.dump(2);
}

std::string app::test_telegram_json() {
  std::string err;
  bool ok = workflow_ptr->test_telegram(err);
  nlohmann::json out = {
      {"ok", ok},
      {"token_configured", !cfg.telegram.bot_token.empty()},
      {"chat_id_configured", !cfg.telegram.chat_id.empty()},
      {"proxy_configured", !cfg.telegram.proxy_url.empty()},
      {"proxy_url_redacted", redact_proxy_url(cfg.telegram.proxy_url)},
      {"ip_resolve", "ipv4"},
      {"timeout_seconds", 30},
      {"message", ok ? "Telegram test message sent" : ""},
      {"curl_error", ok ? "" : err},
      {"http_status", 0},
      {"error", ok ? "" : err}
  };
  return out.dump(2);
}

std::string app::inspect_form_url_json(const std::string& body) {
  nlohmann::json parsed;
  std::string err;
  if (!json_util::parse(body.empty() ? "{}" : body, parsed, &err)) {
    return api_error(err).dump(2);
  }
  std::string url = parsed.value("url", "");
  bool debug = parsed.value("debug", false);
  if (url.empty()) return api_error("url is required").dump(2);
  std::string reason;
  if (!is_allowed_url(url, cfg.security, reason)) {
    return api_error(reason, {{"url", sanitize_url_for_log(url)}}).dump(2);
  }
  auto snapshot = browser_ptr->inspect_form(url, err, debug);
  if (!snapshot.has_value()) {
    return api_error(err.empty() ? "inspect failed" : err).dump(2);
  }
  nlohmann::json out = form_snapshot_to_json(*snapshot);
  if (!snapshot->session_id.empty()) {
    std::string close_err;
    browser_ptr->close_session(snapshot->session_id, close_err);
  }
  return api_success("form inspected", out).dump(2);
}

std::string app::create_form_session_from_url_json(const std::string& body) {
  nlohmann::json parsed;
  std::string err;
  if (!json_util::parse(body.empty() ? "{}" : body, parsed, &err)) {
    return api_error(err).dump(2);
  }
  std::string url = parsed.value("url", "");
  std::string title = parsed.value("title", "Manual test form");
  bool debug = parsed.value("debug", true);
  if (url.empty()) return api_error("url is required").dump(2);

  std::string reason;
  if (!is_allowed_url(url, cfg.security, reason)) {
    return api_error(reason, {{"url", sanitize_url_for_log(url)}}).dump(2);
  }
  if (lower_ascii(cfg.security.mode) == "paranoid") {
    form_session manual;
    manual.mailbox_id = "manual";
    manual.message_uid = "manual-" + std::to_string(std::time(nullptr));
    manual.status = "manual_required";
    manual.form_url = url;
    manual.form_type = "manual";
    manual.title = title;
    std::string id = storage_ptr->create_form_session(manual);
    append_event("info", "paranoid_manual_required", "Paranoid mode manual required",
                 {{"session_id", id}, {"url", sanitize_url_for_log(url)}});
    return api_success("manual session created in paranoid mode",
                       {{"session_id", id}, {"status", "manual_required"}}).dump(2);
  }

  auto snapshot = browser_ptr->inspect_form(url, err, debug);
  if (!snapshot.has_value()) {
    form_session manual;
    manual.mailbox_id = "manual";
    manual.message_uid = "manual-" + std::to_string(std::time(nullptr));
    manual.status = "manual_required";
    manual.form_url = url;
    manual.form_type = "unknown";
    manual.title = title;
    std::string id = storage_ptr->create_form_session(manual);
    append_event("error", "manual_form_inspect_failed", err, {{"session_id", id}, {"url", sanitize_url_for_log(url)}});
    return api_error(err.empty() ? "inspect failed" : err,
                     {{"session_id", id}, {"status", "manual_required"}}).dump(2);
  }

  message msg;
  msg.uid = "manual-" + std::to_string(std::time(nullptr));
  msg.mailbox_id = "manual";
  msg.provider = "manual";
  msg.subject = title;
  msg.body_text = "Manual form: " + url;
  msg.links.push_back({url, "", 0.95});

  form_session session;
  session.mailbox_id = msg.mailbox_id;
  session.message_uid = msg.uid;
  session.form_url = snapshot->url.empty() ? url : snapshot->url;
  session.form_type = snapshot->form_type;
  session.title = snapshot->title.empty() ? title : snapshot->title;
  session.browser_session_id = snapshot->session_id;
  if (snapshot->auth_required) {
    session.status = "waiting_auth";
    session.auth_state_json = nlohmann::json({{"state", "required"}, {"url", snapshot->final_url}}).dump();
  } else if (snapshot->fields.empty()) {
    session.status = "manual_required";
  } else {
    session.status = "waiting_user_review";
    session.fields = llm_ptr->map_fields(msg, *snapshot, profile);
  }
  std::string id = storage_ptr->create_form_session(session);
  auto saved = storage_ptr->get_form_session(id);
  if (saved && saved->status == "waiting_user_review") {
    std::string send_err;
    workflow_ptr->send_form_review(*saved, send_err);
  }
  append_event("info", "manual_form_session_created", "Manual form session created",
               {{"session_id", id}, {"status", session.status}, {"url", sanitize_url_for_log(url)}});
  return api_success("manual form session created",
                     {{"session_id", id}, {"status", session.status}}).dump(2);
}

std::string app::remap_form_json(const std::string& id, const std::string& body) {
  auto session = storage_ptr->get_form_session(id);
  if (!session) return api_error("form session not found").dump(2);
  nlohmann::json request = nlohmann::json::object();
  std::string parse_err;
  if (!body.empty() && !json_util::parse(body, request, &parse_err)) {
    return api_error(parse_err).dump(2);
  }
  form_understanding_options options;
  options.force = request.value("force", false);
  options.preserve_user_edits = !options.force;
  bool use_llm = request.value("use_llm", true);

  message msg;
  msg.uid = session->message_uid;
  msg.mailbox_id = session->mailbox_id;
  msg.subject = session->title;
  msg.body_text = "Form: " + session->form_url;
  msg.links.push_back({session->form_url, "", 0.95});
  form_snapshot snapshot;
  snapshot.url = session->form_url;
  snapshot.form_type = session->form_type;
  snapshot.title = session->title;
  snapshot.fields = session->fields;

  std::map<std::string, form_field> previous_by_id;
  for (const auto& field : session->fields) {
    if (!field.id.empty()) previous_by_id[field.id] = field;
  }
  std::unique_ptr<llm_client> noop;
  llm_client* mapper = llm_ptr.get();
  if (!use_llm) {
    noop = make_noop_llm_client();
    mapper = noop.get();
  }
  auto remapped = mapper->map_fields(msg, snapshot, profile);
  finalize_form_understanding(remapped, profile, previous_by_id, options);
  nlohmann::json diff = nlohmann::json::array();
  for (auto& field : remapped) {
    auto it = previous_by_id.find(field.id);
    std::string old_value = it == previous_by_id.end() ? "" : it->second.value;
    std::string old_source = it == previous_by_id.end() ? "" : it->second.source;
    diff.push_back({
        {"field_id", field.id},
        {"label", field.label},
        {"old_value", old_value},
        {"new_value", field.value},
        {"old_source", old_source},
        {"new_source", field.source},
        {"mapped_profile_key", field.mapped_profile_key},
        {"semantic_key", field.semantic_key},
        {"confidence", field.confidence},
        {"reason", field.reason},
        {"risk", field.risk},
        {"changed", old_value != field.value || old_source != field.source}
    });
  }
  auto validation = validate_understood_fields(remapped);
  auto summary = mapping_summary_to_json(remapped, validation);
  session->fields = std::move(remapped);
  storage_ptr->update_form_session(*session);
  append_event("info", "form_remapped", "Form fields remapped", {{"session_id", id}});
  auto saved = storage_ptr->get_form_session(id);
  nlohmann::json data = {
      {"diff", diff},
      {"fields", saved ? form_session_to_json(*saved)["fields"] : nlohmann::json::array()},
      {"form", saved ? form_session_to_json(*saved) : nlohmann::json::object()},
      {"summary", summary},
      {"validation", validation_to_json(validation)}
  };
  nlohmann::json out = api_success("fields remapped", data);
  out["diff"] = diff;
  out["form"] = data["form"];
  out["summary"] = summary;
  return out.dump(2);
}

std::string app::explain_form_field_json(const std::string& id, const std::string& body) {
  auto session = storage_ptr->get_form_session(id);
  if (!session) return api_error("form session not found").dump(2);
  nlohmann::json parsed;
  std::string err;
  if (!json_util::parse(body.empty() ? "{}" : body, parsed, &err)) return api_error(err).dump(2);
  std::string field_id = parsed.value("field_id", "");
  if (field_id.empty()) return api_error("field_id is required").dump(2);
  for (const auto& field : session->fields) {
    if (field.id != field_id) continue;
    std::string why;
    if (field.requires_user_input) {
      why = field.required ? "required field is empty or needs confirmation" : "field needs optional user choice";
    }
    if (!field.validation_error.empty()) why = field.validation_error;
    nlohmann::json data = {
        {"field_id", field.id},
        {"label", field.label},
        {"reason", field.reason},
        {"source", field.source},
        {"confidence", field.confidence},
        {"risk", field.risk},
        {"why_needs_input", why},
        {"suggested_next_action", field.can_auto_fill ? "review value and validate" : "answer manually or use Web UI"}
    };
    return api_success("field explanation", data).dump(2);
  }
  return api_error("field not found").dump(2);
}

std::string app::validate_form_json(const std::string& id) {
  auto session = storage_ptr->get_form_session(id);
  if (!session) return api_error("form session not found").dump(2);

  auto validation = validate_understood_fields(session->fields);
  auto validation_json = validation_to_json(validation);
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : session->fields) {
    std::string error;
    auto find_error = [&](const nlohmann::json& arr) {
      for (const auto& item : arr) {
        if (item.value("field_id", "") == field.id) return item.value("error", "");
      }
      return std::string{};
    };
    error = find_error(validation_json["missing_required"]);
    if (error.empty()) error = find_error(validation_json["invalid_options"]);
    if (error.empty()) error = find_error(validation_json["unsupported_required"]);
    fields.push_back({
        {"id", field.id},
        {"label", field.label},
        {"ok", error.empty()},
        {"error", error},
        {"required", field.required},
        {"requires_user_input", field.requires_user_input},
        {"value", field.value},
        {"source", field.source},
        {"confidence", field.confidence},
        {"reason", field.reason},
        {"risk", field.risk}
    });
  }
  nlohmann::json data = validation_json;
  data["valid"] = validation.can_fill;
  data["fields"] = fields;
  data["summary"] = mapping_summary_to_json(session->fields, validation);
  nlohmann::json out = api_success(validation.can_fill ? "validation passed" : "validation found issues", data);
  for (auto it = data.begin(); it != data.end(); ++it) out[it.key()] = it.value();
  return out.dump(2);
}

bool app::create_demo_form(bool auth_demo, std::string& err) {
  std::string url = cfg.browser_worker.endpoint + (auth_demo ? "/demo-auth-form" : "/demo-form");
  std::string title = auth_demo ? "Demo auth form" : "Demo form";
  return workflow_ptr->create_demo_session(url, title, auth_demo, err);
}
