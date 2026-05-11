#include "App.h"

#include "FormProviderRouter.h"
#include "FormUnderstandingEngine.h"
#include "ProfileExpansionService.h"

#include "../infra/GoogleFormsProvider.h"
#include "../infra/YandexFormsProvider.h"
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
#include <optional>
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

static std::optional<double> detect_total_memory_gb() {
#if defined(__linux__)
  std::ifstream meminfo("/proc/meminfo");
  std::string key;
  std::uint64_t value_kb = 0;
  std::string unit;
  while (meminfo >> key >> value_kb >> unit) {
    if (key == "MemTotal:") {
      return static_cast<double>(value_kb) / 1024.0 / 1024.0;
    }
  }
#endif
  return std::nullopt;
}

static nlohmann::json llm_memory_json(const llm_config& cfg, const std::optional<double>& total_gb) {
  nlohmann::json memory = {
      {"detected", total_gb.has_value()},
      {"min_required_gb", cfg.min_memory_gb},
      {"recommended_gb", cfg.recommended_memory_gb},
      {"sufficient", true}
  };
  if (total_gb) {
    memory["total_gb"] = *total_gb;
    memory["sufficient"] = *total_gb >= cfg.min_memory_gb;
  }
  return memory;
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
      {"yandex_option_ids", field.yandex_option_ids},
      {"api_question_id", field.api_question_id},
      {"api_answer_type", field.api_answer_type},
      {"api_option_ids", field.api_option_ids},
      {"provider", field.provider},
      {"submit_strategy", field.submit_strategy},
      {"semantic_key_hint", field.semantic_key_hint},
      {"virtual_field", field.virtual_field},
      {"diagnostic_only", field.diagnostic_only}
  };
}

static nlohmann::json form_session_to_json(const form_session& session) {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : session.fields) fields.push_back(form_field_to_json(field));
  nlohmann::json provider_debug = nlohmann::json::object();
  try {
    provider_debug = nlohmann::json::parse(session.provider_debug_json.empty() ? "{}" : session.provider_debug_json);
  } catch (...) {
    provider_debug = session.provider_debug_json;
  }
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
      {"provider_type", session.provider_type},
      {"provider_name", session.provider_name},
      {"extraction_strategy", session.extraction_strategy},
      {"submit_strategy", session.submit_strategy},
      {"api_form_id", session.api_form_id},
      {"public_form_id", session.public_form_id},
      {"provider_debug", provider_debug},
      {"provider_error", session.provider_error},
      {"captcha_required", session.captcha_required},
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
      {"captcha_required", snapshot.captcha_required},
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

static bool is_provider_session_json(const form_session& session) {
  return is_provider_submit_strategy(session.submit_strategy);
}

static void apply_provider_metadata(form_session& session, const provider_inspect_result& inspect) {
  session.provider_type = inspect.provider;
  session.provider_name = inspect.provider == "yandex_forms" ? "Yandex Forms API" :
                          inspect.provider == "google_forms" ? "Google Forms API" :
                          inspect.provider;
  session.extraction_strategy = inspect.extraction_strategy;
  session.submit_strategy = inspect.submit_strategy;
  session.api_form_id = inspect.api_form_id;
  session.public_form_id = inspect.public_form_id;
  session.provider_debug_json = inspect.debug_json.empty() ? "{}" : inspect.debug_json;
  session.provider_error = inspect.error;
  session.captcha_required = inspect.captcha_required;
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
    auto total_memory_gb = detect_total_memory_gb();
    if (total_memory_gb && *total_memory_gb < this->cfg.llm.min_memory_gb) {
      llm_ptr = make_noop_llm_client();
      nlohmann::json data = llm_memory_json(this->cfg.llm, total_memory_gb);
      data["model"] = this->cfg.llm.model;
      append_event(
          "warn",
          "llm_fallback",
          "Insufficient RAM for local LLM, using NoopLlmClient",
          data
      );
    } else {
      if (this->cfg.llm.startup_probe) {
        std::string probe_err;
        if (!test_ollama_health(this->cfg.llm, probe_err)) {
          nlohmann::json data = {
              {"endpoint", this->cfg.llm.endpoint},
              {"model", this->cfg.llm.model},
              {"healthcheck_timeout_seconds", this->cfg.llm.healthcheck_timeout_seconds},
              {"error", probe_err},
              {"next_action", "Run docker compose --profile llm up -d ollama ollama-init or wait for model pull"}
          };
          append_event("warn", "llm_fallback", "Ollama healthcheck failed at startup; calls will fallback to Noop", data);
        }
      }
      llm_ptr = make_ollama_client(this->cfg.llm);
    }
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

  email_ingestion_ptr = std::make_unique<email_ingestion_service>(
      *this->storage_ptr, this->cfg);
  email_classification_ptr = std::make_unique<email_classification_service>(
      *classifier_ptr, *this->storage_ptr, this->cfg.mail_processing);
  email_decision_ptr = std::make_unique<email_decision_engine>(
      this->cfg.mail_processing);
  notification_ptr = std::make_unique<notification_service>(
      *telegram_bot_ptr, *this->storage_ptr, this->cfg);
  attachment_svc_ptr = std::make_unique<attachment_service>(
      *this->storage_ptr, *telegram_bot_ptr, this->cfg);
  mail_controller_ptr = std::make_unique<telegram_mail_controller>(
      *telegram_bot_ptr, *this->storage_ptr, this->cfg);
  dialog_manager_ptr->set_mail_controller(mail_controller_ptr.get());
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

    if (mailbox.client) {
      std::string server_uidvalidity = mailbox.client->fetch_uid_validity();
      if (!server_uidvalidity.empty()) {
        if (!existing->uid_validity.empty() && server_uidvalidity != existing->uid_validity) {
          append_event(
              "warn",
              "uidvalidity_changed",
              "UIDVALIDITY changed — mailbox was rebuilt, resetting checkpoint to start",
              {{"mailbox_id", mailbox_id},
               {"stored", existing->uid_validity},
               {"server", server_uidvalidity}}
          );
          mailbox_checkpoint reset;
          reset.mailbox_id = mailbox_id;
          reset.uid_validity = server_uidvalidity;
          reset.last_seen_uid = 0;
          reset.started_at = now_iso();
          reset.updated_at = reset.started_at;
          storage_ptr->save_checkpoint(reset);
          std::lock_guard<std::mutex> lock(mu);
          mailbox.last_seen_uid = 0;
          status.mailbox_id = mailbox_id;
          status.last_seen_uid = 0;
          return reset;
        }
        if (existing->uid_validity.empty()) {
          existing->uid_validity = server_uidvalidity;
          storage_ptr->save_checkpoint(*existing);
        }
      }
    }
    std::lock_guard<std::mutex> lock(mu);
    mailbox.last_seen_uid = existing->last_seen_uid;
    status.mailbox_id = existing->mailbox_id;
    status.last_seen_uid = existing->last_seen_uid;
    return existing.value();
  }

  mailbox_checkpoint checkpoint;
  checkpoint.mailbox_id = mailbox_id;
  checkpoint.last_seen_uid = 0;
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

      std::uint64_t min_suspect_uid = UINT64_MAX;

      std::cout << "[mail] poll mailbox=" << checkpoint.mailbox_id
                << " last_seen_uid=" << checkpoint.last_seen_uid << std::endl;

      auto fetch_result = mailbox.client->fetch_after_uid_result(checkpoint.last_seen_uid);

      std::cout << "[mail] fetched mailbox=" << checkpoint.mailbox_id
                << " searched=" << fetch_result.searched_uids.size()
                << " messages=" << fetch_result.messages.size()
                << " failed=" << fetch_result.failed_uids.size()
                << " parse_failed=" << fetch_result.parse_failed_uids.size() << std::endl;


      for (const auto& fuid : fetch_result.failed_uids) {
        std::uint64_t n = parse_uid_or_zero(fuid);
        if (n > 0 && n < min_suspect_uid) min_suspect_uid = n;
        append_event("warn", "imap_fetch_failed",
            "IMAP FETCH returned a network error — checkpoint not advanced",
            {{"uid", fuid}, {"mailbox_id", checkpoint.mailbox_id}});
      }


      for (const auto& pfuid : fetch_result.parse_failed_uids) {
        std::uint64_t n = parse_uid_or_zero(pfuid);
        if (n > 0 && n < min_suspect_uid) min_suspect_uid = n;
        append_event("warn", "imap_message_parse_suspect",
            "IMAP message had empty subject/from/body — checkpoint not advanced",
            {{"uid", pfuid}, {"mailbox_id", checkpoint.mailbox_id}});
      }


      for (auto msg : fetch_result.messages) {
        if (msg.mailbox_id.empty() || msg.mailbox_id == "default") {
          msg.mailbox_id = checkpoint.mailbox_id;
        }
        if (msg.provider.empty()) msg.provider = mailbox.cfg.provider;

        std::uint64_t numeric_uid = parse_uid_or_zero(msg.uid);

        if (storage_ptr->is_processed(msg.mailbox_id, msg.uid)) {
          std::cout << "[mail] skip uid=" << msg.uid << " already_processed" << std::endl;
          if (numeric_uid > max_seen) max_seen = numeric_uid;
          continue;
        }

        std::cout << "[mail] process uid=" << msg.uid
                  << " from=" << msg.from
                  << " subject=" << msg.subject << std::endl;

        append_event("info", "mail_fetched", "Email fetched from IMAP",
            {{"uid", msg.uid}, {"mailbox_id", msg.mailbox_id},
             {"subject", msg.subject}, {"from", msg.from},
             {"links", static_cast<int>(msg.links.size())},
             {"attachments", static_cast<int>(msg.attachments.size())}});


        std::string email_id;
        email_analysis analysis;
        email_decision decision;
        bool pipeline_ok = false;

        if (email_ingestion_ptr) {
          email_id = email_ingestion_ptr->ingest(msg);
          append_event("info", "mail_stored", "Email stored in database",
              {{"uid", msg.uid}, {"email_id", email_id}});
        }

        if (!email_id.empty()) {
          if (attachment_svc_ptr) attachment_svc_ptr->store_attachments(email_id, msg);
          if (email_classification_ptr) {
            append_event("info", "mail_classification_started", "Email classification started",
                {{"uid", msg.uid}, {"email_id", email_id}});
            analysis = email_classification_ptr->classify(email_id, msg);
            append_event("info", "mail_classification_finished", "Email classification finished",
                {{"uid", msg.uid}, {"kind", to_string(analysis.kind)},
                 {"level", to_string(analysis.level)},
                 {"confidence", analysis.confidence},
                 {"should_notify", analysis.should_notify}});
          }
          if (email_decision_ptr) {
            decision = email_decision_ptr->decide(analysis, msg);
            std::string action_str =
                decision.action == email_action::notify    ? "notify"    :
                decision.action == email_action::form_fill ? "form_fill" : "ignore";
            append_event("info", "mail_decision_created", "Email decision made",
                {{"uid", msg.uid}, {"action", action_str}, {"reason", decision.reason}});
          }
          pipeline_ok = true;
        }

        if (pipeline_ok) {
          if (decision.action == email_action::form_fill) {

            auto wf_result = workflow_ptr->handle_message(msg, rules_copy);
            std::cout << "[mail] form_fill uid=" << msg.uid
                      << " matched=" << wf_result.matched << std::endl;
            if (wf_result.matched) matched++;
          } else if (decision.action == email_action::notify) {
            if (notification_ptr) {
              auto stored = storage_ptr->get_email_message(email_id);
              if (stored) {
                notification_ptr->notify_email(*stored, analysis);
                append_event("info", "mail_important_notified", "Telegram notification sent",
                    {{"uid", msg.uid}, {"email_id", email_id},
                     {"importance_level", stored->importance_level}});
              }
            }
            storage_ptr->mark_processed(msg, "important_notified");
            matched++;
          } else {
            storage_ptr->mark_processed(msg, "ignored");
            append_event("info", "mail_ignored", "Email classified as ignored",
                {{"uid", msg.uid}, {"reason", decision.reason}});
          }
          if (cfg.mail_processing.mark_seen_after_success && mailbox.client) {
            mailbox.client->mark_message_seen(msg.uid);
          }

          if (numeric_uid > max_seen) max_seen = numeric_uid;
        } else {

          auto wf_result = workflow_ptr->handle_message(msg, rules_copy);
          std::cout << "[mail] legacy result uid=" << msg.uid
                    << " matched=" << wf_result.matched << std::endl;
          if (wf_result.matched) matched++;
          if (numeric_uid > max_seen) max_seen = numeric_uid;
        }
      }


      std::uint64_t safe_max = (min_suspect_uid != UINT64_MAX && min_suspect_uid > 0)
          ? std::min(max_seen, min_suspect_uid - 1)
          : max_seen;

      if (safe_max > checkpoint.last_seen_uid) {
        checkpoint.last_seen_uid = safe_max;
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
  j["form_providers"] = {
      {"prefer_provider_api", cfg.form_providers.prefer_provider_api},
      {"browser_fallback_for_known_providers", cfg.form_providers.browser_fallback_for_known_providers},
      {"yandex_api_enabled", cfg.yandex_forms_api.enabled},
      {"yandex_mapping_file", cfg.yandex_forms_api.form_map_file},
      {"google_api_enabled", cfg.google_forms_api.enabled},
      {"google_mapping_file", cfg.google_forms_api.form_map_file}
  };
  j["llm"] = {
      {"enabled", cfg.llm.enabled},
      {"provider", cfg.llm.provider},
      {"model", cfg.llm.model},
      {"endpoint", cfg.llm.endpoint},
      {"auto_fallback_to_noop", cfg.llm.auto_fallback_to_noop},
      {"startup_probe", cfg.llm.startup_probe},
      {"memory", llm_memory_json(cfg.llm, detect_total_memory_gb())}
  };
  j["web"] = {{"enabled", cfg.http.enabled}, {"host", cfg.http.host}, {"port", cfg.http.port},
              {"web_public_base_url", cfg.http.web_public_base_url}};
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
  out["form_providers"] = {
      {"prefer_provider_api", cfg.form_providers.prefer_provider_api},
      {"browser_fallback_for_known_providers", cfg.form_providers.browser_fallback_for_known_providers}
  };
  out["yandex_forms_api"] = {
      {"enabled", cfg.yandex_forms_api.enabled},
      {"base_url", cfg.yandex_forms_api.base_url},
      {"oauth_token_configured", !cfg.yandex_forms_api.oauth_token.empty()},
      {"org_id_configured", !cfg.yandex_forms_api.org_id.empty()},
      {"cloud_org_id_configured", !cfg.yandex_forms_api.cloud_org_id.empty()},
      {"form_map_file", cfg.yandex_forms_api.form_map_file},
      {"dry_run", cfg.yandex_forms_api.dry_run},
      {"allow_browser_fallback", cfg.yandex_forms_api.allow_browser_fallback},
      {"timeout_seconds", cfg.yandex_forms_api.timeout_seconds}
  };
  out["google_forms_api"] = {
      {"enabled", cfg.google_forms_api.enabled},
      {"credentials_json_configured", !cfg.google_forms_api.credentials_json.empty()},
      {"oauth_token_configured", !cfg.google_forms_api.oauth_token.empty()},
      {"form_map_file", cfg.google_forms_api.form_map_file},
      {"dry_run", cfg.google_forms_api.dry_run},
      {"allow_browser_fallback", cfg.google_forms_api.allow_browser_fallback},
      {"timeout_seconds", cfg.google_forms_api.timeout_seconds}
  };
  out["llm"] = {
      {"enabled", cfg.llm.enabled},
      {"provider", cfg.llm.provider},
      {"endpoint", cfg.llm.endpoint},
      {"endpoint_env", cfg.llm.endpoint_env},
      {"model", cfg.llm.model},
      {"model_env", cfg.llm.model_env},
      {"privacy_mode", cfg.llm.privacy_mode},
      {"timeout_seconds", cfg.llm.timeout_seconds},
      {"healthcheck_timeout_seconds", cfg.llm.healthcheck_timeout_seconds},
      {"auto_fallback_to_noop", cfg.llm.auto_fallback_to_noop},
      {"auto_pull", cfg.llm.auto_pull},
      {"startup_probe", cfg.llm.startup_probe},
      {"min_memory_gb", cfg.llm.min_memory_gb},
      {"recommended_memory_gb", cfg.llm.recommended_memory_gb}
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
  out["web"] = {
      {"enabled", cfg.http.enabled},
      {"host", cfg.http.host},
      {"port", cfg.http.port},
      {"auth_token_configured", !cfg.http.auth_token.empty()},
      {"web_public_base_url", cfg.http.web_public_base_url}
  };
  out["telegram"]["captcha_remote_control_experimental"] = cfg.telegram.captcha_remote_control_experimental;
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
  bool model_ready = false;
  bool fallback = false;
  std::string warning;
  std::string next_action;
  std::unique_ptr<llm_client> test_client;
  std::string active_client = "NoopLlmClient";
  double total_duration_ms = 0.0;
  auto total_memory_gb = detect_total_memory_gb();
  bool memory_sufficient = !total_memory_gb || *total_memory_gb >= cfg.llm.min_memory_gb;

  if (cfg.llm.enabled && !memory_sufficient) {
    fallback = true;
    std::ostringstream ss;
    ss << "Insufficient RAM for default model " << cfg.llm.model
       << ": need at least " << cfg.llm.min_memory_gb << "GB";
    warning = ss.str();
    next_action = "Use Noop fallback, choose a smaller model, or run on a machine with more RAM";
    test_client = make_noop_llm_client();
  } else if (cfg.llm.enabled) {
    auto probe = probe_ollama_endpoint(cfg.llm);
    reachable = probe.reachable;
    model_ready = probe.model_ready;
    total_duration_ms = probe.total_duration_ms;
    err = probe.error;
    if (probe.reachable && probe.model_ready) {
      test_client = make_ollama_client(cfg.llm);
      active_client = "OllamaClient";
    } else {
      fallback = true;
      warning = err.empty() ? "Ollama model is unavailable; using rule-based mapping" :
                              "Ollama model is unavailable; using rule-based mapping: " + err;
      next_action = "Run: docker compose --profile llm up -d ollama ollama-init, or wait for model pull";
      nlohmann::json data = {
          {"endpoint", cfg.llm.endpoint},
          {"model", cfg.llm.model},
          {"timeout_seconds", cfg.llm.timeout_seconds},
          {"healthcheck_timeout_seconds", cfg.llm.healthcheck_timeout_seconds},
          {"total_duration_ms", probe.total_duration_ms},
          {"error", err},
          {"next_action", next_action}
      };
      append_event("warn", "llm_fallback", "Ollama unavailable, using NoopLlmClient", data);
      test_client = make_noop_llm_client();
    }
  } else {
    warning = "Local LLM is disabled; using rule-based mapping";
    next_action = "Set LLM_ENABLED=true and run docker compose --profile llm up --build";
    test_client = make_noop_llm_client();
  }

  message msg = make_sample_form_message();
  auto sample_client = make_noop_llm_client();
  auto analysis = sample_client->analyze_email(msg);
  form_snapshot sample_form = make_sample_mapping_form();
  auto mapped = sample_client->map_fields(msg, sample_form, make_sample_profile());

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
  if (!sample_mapping_ok) {
    fallback = true;
    active_client = "NoopLlmClient";
    model_ready = false;
    auto noop = make_noop_llm_client();
    mapped = noop->map_fields(msg, sample_form, make_sample_profile());
    sample_mapping_ok = mapping_ok(mapped);
    warning = "Ollama sample mapping failed validation; using rule-based mapping";
    next_action = "Check model JSON-mode behavior or try another local model";
    nlohmann::json data = {{"model", cfg.llm.model}, {"endpoint", cfg.llm.endpoint}};
    append_event("warn", "llm_fallback", "Ollama sample mapping failed validation, using NoopLlmClient", data);
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

  bool ok = sample_mapping_ok && analysis.kind == message_kind::form_request;
  if (cfg.llm.enabled && fallback && !cfg.llm.auto_fallback_to_noop) ok = false;
  if (cfg.llm.enabled && !fallback && total_duration_ms > 60000.0) {
    warning = "Ollama is working but slow on CPU; consider a smaller model or higher timeout.";
  }

  nlohmann::json out = {
      {"ok", ok},
      {"enabled", cfg.llm.enabled},
      {"provider", cfg.llm.enabled ? cfg.llm.provider : "noop"},
      {"active_client", active_client},
      {"model", cfg.llm.model},
      {"endpoint", cfg.llm.endpoint},
      {"reachable", reachable},
      {"model_ready", model_ready},
      {"fallback", fallback},
      {"auto_fallback_to_noop", cfg.llm.auto_fallback_to_noop},
      {"timeout_seconds", cfg.llm.timeout_seconds},
      {"healthcheck_timeout_seconds", cfg.llm.healthcheck_timeout_seconds},
      {"total_duration_ms", total_duration_ms},
      {"sample_classification", message_kind_name(analysis.kind)},
      {"sample_mapping_ok", sample_mapping_ok},
      {"sample_mapping_source", "NoopLlmClient deterministic sample"},
      {"mapped_count", mapped_count},
      {"needs_input_count", needs_input_count},
      {"invalid_json_count", 0},
      {"invalid_schema_count", 0},
      {"last_llm_error", (cfg.llm.enabled && (!reachable || !model_ready)) ? err : ""},
      {"fallback_used", fallback},
      {"warning", warning},
      {"next_action", next_action},
      {"memory", llm_memory_json(cfg.llm, total_memory_gb)},
      {"mapped_fields", mapped_fields},
      {"error", (cfg.llm.enabled && (!reachable || !model_ready)) ? err : ""}
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
  form_provider_router router(cfg);
  provider_route route = router.route_for_url(url);
  if (route.provider_type == form_provider_type::yandex_forms) {
    yandex_forms_provider provider(cfg.yandex_forms_api);
    auto inspected = provider.inspect(url);
    nlohmann::json out = form_snapshot_to_json(inspected.snapshot);
    out["provider_type"] = inspected.provider;
    out["provider_name"] = "Yandex Forms";
    out["extraction_strategy"] = inspected.extraction_strategy.empty() ? "manual" : inspected.extraction_strategy;
    out["submit_strategy"] = inspected.submit_strategy.empty() ? "manual" : inspected.submit_strategy;
    out["public_form_id"] = inspected.public_form_id;
    out["api_form_id"] = inspected.api_form_id;
    out["manual_required"] = inspected.manual_required;
    out["captcha_required"] = inspected.captcha_required;
    out["provider_error"] = inspected.error;
    out["browser_fallback_allowed"] = route.allow_browser_fallback;
    if (!inspected.ok) {
      if (route.allow_browser_fallback && browser_ptr) {
        std::string berr;
        auto snap = browser_ptr->inspect_form(url, berr, debug);
        if (snap.has_value()) {
          nlohmann::json bout = form_snapshot_to_json(*snap);
          bout["provider_type"] = "yandex_forms";
          bout["provider_name"] = "Yandex Forms";
          bout["extraction_strategy"] = snap->captcha_required ? "captcha_blocked" : "browser_dom";
          bout["submit_strategy"] = snap->captcha_required ? "manual" : "browser_worker";
          bout["browser_fallback_used"] = true;
          bout["provider_error"] = inspected.error;
          if (!snap->session_id.empty()) { std::string ce; browser_ptr->close_session(snap->session_id, ce); }
          return api_success("form inspected via browser fallback", bout).dump(2);
        }
      }
      return api_error(inspected.error.empty() ? "provider inspect failed" : inspected.error, out).dump(2);
    }
    return api_success("form inspected through provider", out).dump(2);
  }
  if (route.provider_type == form_provider_type::google_forms) {
    google_forms_provider provider(cfg.google_forms_api);
    auto inspected = provider.inspect(url);
    nlohmann::json out = form_snapshot_to_json(inspected.snapshot);
    out["provider_type"] = inspected.provider;
    out["provider_name"] = "Google Forms";
    out["extraction_strategy"] = inspected.extraction_strategy.empty() ? "manual" : inspected.extraction_strategy;
    out["submit_strategy"] = inspected.submit_strategy.empty() ? "manual" : inspected.submit_strategy;
    out["public_form_id"] = inspected.public_form_id;
    out["api_form_id"] = inspected.api_form_id;
    out["manual_required"] = inspected.manual_required;
    out["captcha_required"] = inspected.captcha_required;
    out["provider_error"] = inspected.error;
    out["browser_fallback_allowed"] = route.allow_browser_fallback;
    if (!inspected.ok) {
      if (route.allow_browser_fallback && browser_ptr) {
        std::string berr;
        auto snap = browser_ptr->inspect_form(url, berr, debug);
        if (snap.has_value()) {
          nlohmann::json bout = form_snapshot_to_json(*snap);
          bout["provider_type"] = "google_forms";
          bout["provider_name"] = "Google Forms";
          bout["extraction_strategy"] = snap->captcha_required ? "captcha_blocked" : "browser_dom";
          bout["submit_strategy"] = snap->captcha_required ? "manual" : "browser_worker";
          bout["browser_fallback_used"] = true;
          bout["provider_error"] = inspected.error;
          if (!snap->session_id.empty()) { std::string ce; browser_ptr->close_session(snap->session_id, ce); }
          return api_success("form inspected via browser fallback", bout).dump(2);
        }
      }
      return api_error(inspected.error.empty() ? "provider inspect failed" : inspected.error, out).dump(2);
    }
    return api_success("form inspected through provider", out).dump(2);
  }
  auto snapshot = browser_ptr->inspect_form(url, err, debug);
  if (!snapshot.has_value()) {
    return api_error(err.empty() ? "inspect failed" : err).dump(2);
  }
  nlohmann::json out = form_snapshot_to_json(*snapshot);
  out["provider_type"] = "generic_browser";
  out["provider_name"] = "Generic Browser";
  out["extraction_strategy"] = snapshot->captcha_required ? "captcha_blocked" : "browser_dom";
  out["submit_strategy"] = snapshot->captcha_required ? "manual" : "browser_worker";
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

  form_provider_router router(cfg);
  provider_route route = router.route_for_url(url);
  if (route.provider_type == form_provider_type::yandex_forms || route.provider_type == form_provider_type::google_forms) {
    provider_inspect_result inspected;
    if (route.provider_type == form_provider_type::yandex_forms) {
      yandex_forms_provider provider(cfg.yandex_forms_api);
      inspected = provider.inspect(url);
    } else {
      google_forms_provider provider(cfg.google_forms_api);
      inspected = provider.inspect(url);
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
    session.form_url = inspected.snapshot.url.empty() ? url : inspected.snapshot.url;
    session.form_type = route.provider_type == form_provider_type::yandex_forms ? "yandex_forms" : "google_forms";
    session.title = inspected.snapshot.title.empty() ? title : inspected.snapshot.title;
    apply_provider_metadata(session, inspected);
    if (inspected.ok) {
      session.status = "waiting_user_review";
      form_snapshot mapped = inspected.snapshot;
      mapped.url = session.form_url;
      mapped.title = session.title;
      mapped.debug_json = inspected.debug_json;
      session.fields = llm_ptr->map_fields(msg, mapped, profile);
      std::string id = storage_ptr->create_form_session(session);
      auto saved = storage_ptr->get_form_session(id);
      if (saved && saved->status == "waiting_user_review") {
        std::string send_err;
        workflow_ptr->send_form_review(*saved, send_err);
      }
      append_event("info", "provider_form_session_created", "Provider form session created",
                   {{"session_id", id}, {"provider", session.provider_type}, {"status", session.status}});
      return api_success("provider form session created",
                         {{"session_id", id}, {"status", session.status}, {"provider_type", session.provider_type}}).dump(2);
    }
    if (!route.allow_browser_fallback || !browser_ptr) {
      session.status = "manual_required";
      session.submit_strategy = "manual";
      session.provider_error = inspected.error;
      std::string id = storage_ptr->create_form_session(session);
      append_event("warning", "provider_manual_required", inspected.error,
                   {{"session_id", id}, {"provider", session.provider_type}, {"browser_fallback_allowed", false}});
      nlohmann::json details = {
          {"session_id", id},
          {"status", session.status},
          {"provider_type", session.provider_type},
          {"provider_error", inspected.error},
          {"browser_fallback_allowed", false}
      };
      return api_error(inspected.error.empty() ? "provider setup required" : inspected.error, details).dump(2);
    }

    append_event("info", "provider_failed_browser_fallback", "Provider check failed, trying browser fallback",
                 {{"provider", session.provider_type}, {"url", sanitize_url_for_log(url)}, {"error", inspected.error}});
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
    manual.provider_type = "generic_browser";
    manual.provider_name = "Generic Browser";
    manual.submit_strategy = "manual";
    manual.provider_error = err;
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
  session.provider_type = "generic_browser";
  session.provider_name = "Generic Browser";
  session.extraction_strategy = snapshot->captcha_required ? "captcha_blocked" : "browser_dom";
  session.submit_strategy = "browser_worker";
  session.provider_debug_json = snapshot->debug_json;
  session.captcha_required = snapshot->captcha_required;
  if (snapshot->auth_required) {
    session.status = "waiting_auth";
    session.auth_state_json = nlohmann::json({{"state", "required"}, {"url", snapshot->final_url}}).dump();
  } else if (snapshot->captcha_required) {
    session.status = "captcha_required";
    session.captcha_required = true;
  } else if (snapshot->fields.empty()) {
    session.status = "manual_required";
  } else {
    session.status = "waiting_user_review";
    session.fields = llm_ptr->map_fields(msg, *snapshot, profile);
  }
  std::string id = storage_ptr->create_form_session(session);
  auto saved = storage_ptr->get_form_session(id);
  if (saved) {
    std::string send_err;
    if (saved->status == "waiting_user_review") {
      workflow_ptr->send_form_review(*saved, send_err);
    } else if (saved->status == "captcha_required") {
      workflow_ptr->send_captcha_message(*saved, send_err);
    }
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

bool app::create_demo_captcha_form(std::string& err) {
  std::string url = cfg.browser_worker.endpoint + "/demo-captcha-then-form";
  return workflow_ptr->create_demo_session(url, "Demo captcha then form", false, err);
}

std::string app::form_screenshot_png(const std::string& id) {
  auto session = storage_ptr->get_form_session(id);
  if (!session || session->browser_session_id.empty()) return {};
  std::string err;
  return browser_ptr->get_screenshot_png(session->browser_session_id, err);
}

bool app::captcha_click_form(const std::string& id, const std::string& body, std::string& err) {
  auto session = storage_ptr->get_form_session(id);
  if (!session) { err = "form session not found"; return false; }
  if (session->browser_session_id.empty()) { err = "no browser session"; return false; }
  nlohmann::json parsed;
  if (!json_util::parse(body.empty() ? "{}" : body, parsed, &err)) return false;
  int x = parsed.value("x", 0);
  int y = parsed.value("y", 0);
  return browser_ptr->click_at(session->browser_session_id, x, y, err);
}

bool app::captcha_reinspect_form(const std::string& id, std::string& err) {
  return workflow_ptr->reinspect_after_captcha(id, err);
}

std::string app::mail_debug_json() const {
  nlohmann::json out;
  out["timestamp"] = now_iso();
  out["mailboxes"] = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& mailbox : mailboxes) {
      std::string masked_user = mailbox.cfg.username;
      if (masked_user.size() > 2) masked_user = masked_user.substr(0, 2) + "***";
      out["mailboxes"].push_back({
          {"id", mailbox.cfg.mailbox_id},
          {"provider", mailbox.cfg.provider},
          {"host", mailbox.cfg.host},
          {"port", mailbox.cfg.port},
          {"tls", mailbox.cfg.tls},
          {"folder", mailbox.cfg.folder},
          {"username", masked_user},
          {"enabled", mailbox.client != nullptr},
          {"last_error", mailbox.last_error},
          {"last_seen_uid", mailbox.last_seen_uid},
          {"last_check", mailbox.last_check},
          {"matched_last", mailbox.matched_last}
      });
    }
  }
  out["processed_total"] = storage_ptr ? storage_ptr->processed_count() : 0;
  return out.dump(2);
}

std::string app::mail_scan_last_json(int n) {
  if (n <= 0 || n > 100) n = 10;
  nlohmann::json out;
  out["n"] = n;
  out["mailboxes"] = nlohmann::json::array();
  for (auto& mailbox : mailboxes) {
    nlohmann::json box;
    box["id"] = mailbox.cfg.mailbox_id;
    if (!mailbox.client) {
      box["error"] = mailbox.last_error.empty() ? "no mail client" : mailbox.last_error;
      out["mailboxes"].push_back(box);
      continue;
    }
    auto msgs = mailbox.client->fetch_last_n(n);
    box["fetched"] = msgs.size();
    box["messages"] = nlohmann::json::array();
    for (const auto& msg : msgs) {
      bool processed = storage_ptr && storage_ptr->is_processed(
          msg.mailbox_id.empty() ? mailbox.cfg.mailbox_id : msg.mailbox_id, msg.uid);
      box["messages"].push_back({
          {"uid", msg.uid},
          {"from", msg.from},
          {"subject", msg.subject},
          {"date", msg.date_iso},
          {"snippet", msg.snippet},
          {"links", static_cast<int>(msg.links.size())},
          {"already_processed", processed}
      });
    }
    out["mailboxes"].push_back(box);
  }
  return out.dump(2);
}

std::string app::mail_reset_state_json(const std::string& mailbox_id_param) {
  if (!storage_ptr) return api_error("storage not available").dump(2);
  std::string target_id = mailbox_id_param.empty() ? "main" : mailbox_id_param;

  mailbox_checkpoint reset;
  reset.mailbox_id = target_id;
  reset.last_seen_uid = 0;
  reset.started_at = now_iso();
  reset.updated_at = reset.started_at;
  storage_ptr->save_checkpoint(reset);

  {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& mailbox : mailboxes) {
      const std::string& id = mailbox.cfg.mailbox_id.empty() ? "main" : mailbox.cfg.mailbox_id;
      if (id == target_id) {
        mailbox.last_seen_uid = 0;
      }
    }
    if (status.mailbox_id == target_id || status.mailbox_id.empty()) {
      status.last_seen_uid = 0;
    }
  }

  append_event("info", "mail_state_reset", "Mail checkpoint reset to 0",
               {{"mailbox_id", target_id}});
  return nlohmann::json({
      {"ok", true},
      {"message", "checkpoint reset for mailbox " + target_id + "; next poll will re-scan from uid=1"},
      {"mailbox_id", target_id},
      {"last_seen_uid", 0}
  }).dump(2);
}

std::string app::expand_profile_preview_json(const std::string& body) {
  bool use_llm = true;
  try {
    if (!body.empty()) {
      auto parsed = nlohmann::json::parse(body);
      use_llm = parsed.value("use_llm", true);
    }
  } catch (...) {}

  bool llm_available = cfg.llm.enabled && !cfg.llm.endpoint.empty();
  auto suggestions = suggest_profile_expansions(profile, cfg.llm, use_llm && llm_available);

  nlohmann::json out;
  out["ok"] = true;
  out["llm_used"] = use_llm && llm_available;
  out["llm_available"] = llm_available;
  out["suggestions"] = nlohmann::json::array();
  for (const auto& s : suggestions) {
    out["suggestions"].push_back({
      {"key", s.key},
      {"value", s.value},
      {"source", s.source},
      {"reason", s.reason},
      {"confidence", s.confidence}
    });
  }
  append_event("info", "profile_expansion_preview_requested",
    "Profile expansion preview: " + std::to_string(suggestions.size()) + " suggestions",
    {{"count", (int)suggestions.size()}, {"llm_used", use_llm && llm_available}});
  return out.dump(2);
}


static nlohmann::json stored_email_summary_to_json(const stored_email& e) {
  int att_count = 0;
  if (!e.attachments_json.empty() && e.attachments_json != "[]") {
    for (char c : e.attachments_json) if (c == '{') att_count++;
  }
  return {
    {"id", e.id},
    {"mailbox_id", e.mailbox_id},
    {"uid", e.uid},
    {"from", e.from_addr},
    {"subject", e.subject},
    {"date", e.date_iso},
    {"snippet", e.snippet},
    {"importance_level", e.importance_level},
    {"importance_score", e.importance_score},
    {"category", e.category},
    {"status", e.status},
    {"read_at", e.read_at},
    {"archived_at", e.archived_at},
    {"muted_until", e.muted_until},
    {"attachment_count", att_count},
    {"created_at", e.created_at},
    {"updated_at", e.updated_at}
  };
}

static nlohmann::json stored_email_detail_to_json(const stored_email& e) {
  nlohmann::json j = stored_email_summary_to_json(e);
  j["to"] = e.to_addr;
  j["message_id"] = e.message_id;
  j["body_text"] = e.body_text;
  nlohmann::json cls = nlohmann::json::object();
  if (!e.classification_json.empty()) {
    try { cls = nlohmann::json::parse(e.classification_json); } catch (...) {}
  }
  j["classification"] = cls;
  return j;
}

std::string app::mail_list_json(const std::string& filter, int limit, int offset) const {
  if (!storage_ptr) return api_error("storage not available").dump(2);
  email_list_filter f;
  f.status = filter.empty() ? "all" : filter;
  if (limit <= 0 || limit > 100) limit = 20;
  if (offset < 0) offset = 0;
  auto emails = storage_ptr->list_emails(f, limit, offset);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& e : emails) arr.push_back(stored_email_summary_to_json(e));
  return nlohmann::json({{"ok", true}, {"filter", filter}, {"count", (int)arr.size()}, {"emails", arr}}).dump(2);
}

std::string app::mail_get_json(const std::string& id) const {
  if (!storage_ptr) return api_error("storage not available").dump(2);
  auto email = storage_ptr->get_email_message(id);
  if (!email) return api_error("email not found", {{"id", id}}).dump(2);
  nlohmann::json out;
  out["ok"] = true;
  out["email"] = stored_email_detail_to_json(*email);
  return out.dump(2);
}

bool app::mail_mark_read(const std::string& id, std::string& err) {
  if (!storage_ptr) { err = "storage not available"; return false; }
  auto email = storage_ptr->get_email_message(id);
  if (!email) { err = "email not found"; return false; }
  storage_ptr->mark_email_read(id);
  append_event("info", "mail_read", "Email marked as read", {{"email_id", id}});
  return true;
}

bool app::mail_archive(const std::string& id, std::string& err) {
  if (!storage_ptr) { err = "storage not available"; return false; }
  auto email = storage_ptr->get_email_message(id);
  if (!email) { err = "email not found"; return false; }
  storage_ptr->archive_email(id);
  append_event("info", "mail_archived", "Email archived", {{"email_id", id}});
  return true;
}

bool app::mail_mute(const std::string& id, const std::string& body, std::string& err) {
  if (!storage_ptr) { err = "storage not available"; return false; }
  auto email = storage_ptr->get_email_message(id);
  if (!email) { err = "email not found"; return false; }

  nlohmann::json parsed = nlohmann::json::object();
  if (!body.empty()) {
    try { parsed = nlohmann::json::parse(body); } catch (...) {}
  }

  std::string until_iso;
  if (parsed.contains("until") && parsed["until"].is_string()) {
    until_iso = parsed["until"].get<std::string>();
  } else {
    int mute_hours = parsed.value("hours", 24);
    if (mute_hours <= 0 || mute_hours > 24 * 365) mute_hours = 24;
    auto t = system_clock::to_time_t(system_clock::now() + hours(mute_hours));
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    until_iso = buf;
  }

  storage_ptr->mute_email(id, until_iso);
  append_event("info", "mail_muted", "Email muted", {{"email_id", id}, {"until", until_iso}});
  return true;
}

std::string app::mail_attachments_json(const std::string& email_id) const {
  if (!storage_ptr) return api_error("storage not available").dump(2);
  auto email = storage_ptr->get_email_message(email_id);
  if (!email) return api_error("email not found", {{"id", email_id}}).dump(2);
  auto attachments = storage_ptr->get_email_attachments(email_id);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& a : attachments) {
    arr.push_back({
      {"id", a.id},
      {"email_id", a.email_id},
      {"filename", a.filename},
      {"mime_type", a.mime_type},
      {"size_bytes", a.size_bytes},
      {"disposition", a.disposition},
      {"safe_to_preview", a.safe_to_preview},
      {"downloaded", a.downloaded},
      {"sha256", a.sha256},
      {"created_at", a.created_at}
    });
  }
  return nlohmann::json({{"ok", true}, {"email_id", email_id}, {"count", (int)arr.size()}, {"attachments", arr}}).dump(2);
}

bool app::mail_attachment_download(const std::string& attachment_id,
                                    std::string& content,
                                    std::string& content_type,
                                    std::string& filename,
                                    std::string& err) const {
  if (!storage_ptr) { err = "storage not available"; return false; }
  auto att = storage_ptr->get_attachment(attachment_id);
  if (!att) { err = "attachment not found"; return false; }
  if (!att->downloaded || att->local_path.empty()) {
    err = "attachment not yet downloaded from IMAP";
    return false;
  }
  if (!att->safe_to_preview) {
    err = "attachment is not marked safe for download";
    return false;
  }
  std::ifstream f(att->local_path, std::ios::binary);
  if (!f) { err = "attachment file not found on disk"; return false; }
  content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  content_type = att->mime_type.empty() ? "application/octet-stream" : att->mime_type;
  filename = att->filename;
  return true;
}

std::string app::mail_search_json(const std::string& query, int limit, int offset) const {
  if (!storage_ptr) return api_error("storage not available").dump(2);
  if (query.empty()) return api_error("q parameter is required").dump(2);
  if (limit <= 0 || limit > 100) limit = 20;
  if (offset < 0) offset = 0;
  auto emails = storage_ptr->search_emails(query, limit, offset);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& e : emails) arr.push_back(stored_email_summary_to_json(e));
  return nlohmann::json({{"ok", true}, {"query", query}, {"count", (int)arr.size()}, {"emails", arr}}).dump(2);
}

bool app::apply_profile_expansion_json(const std::string& body, std::string& err) {
  nlohmann::json parsed;
  if (!json_util::parse(body, parsed, &err)) return false;

  if (!parsed.contains("selected_keys") || !parsed["selected_keys"].is_array()) {
    err = "selected_keys array is required";
    return false;
  }
  if (!parsed.contains("suggestions") || !parsed["suggestions"].is_array()) {
    err = "suggestions array is required";
    return false;
  }

  std::map<std::string, std::string> key_to_value;
  for (const auto& item : parsed["suggestions"]) {
    if (!item.is_object()) continue;
    std::string k = item.value("key", "");
    std::string v = item.value("value", "");
    if (!k.empty() && !v.empty()) key_to_value[k] = v;
  }

  std::vector<std::pair<std::string, std::string>> to_apply;
  for (const auto& key_item : parsed["selected_keys"]) {
    if (!key_item.is_string()) continue;
    std::string k = key_item.get<std::string>();
    if (!is_expansion_key_allowed(k)) {
      err = "key not in allowlist: " + k;
      return false;
    }
    auto it = key_to_value.find(k);
    if (it == key_to_value.end()) {
      err = "key not found in suggestions: " + k;
      return false;
    }
    to_apply.push_back({k, it->second});
  }

  if (to_apply.empty()) {
    err = "no keys selected";
    return false;
  }

  nlohmann::json backup_json;
  for (const auto& [k, v] : profile.values) backup_json[k] = v;

  nlohmann::json applied_json = nlohmann::json::array();
  for (const auto& [k, v] : to_apply) {
    profile.values[k] = v;
    applied_json.push_back({{"key", k}, {"value", v}});
  }

  std::ofstream f(cfg.profile_file, std::ios::binary);
  if (!f) {
    err = "cannot write profile file";
    return false;
  }
  f << user_profile_to_json(profile);
  f.close();

  if (workflow_ptr) workflow_ptr->set_profile(profile);

  append_event("info", "profile_expansion_applied",
    "Profile expansion applied: " + std::to_string(to_apply.size()) + " fields added",
    {{"applied", applied_json}, {"backup_key_count", static_cast<int>(backup_json.size())}});
  return true;
}
