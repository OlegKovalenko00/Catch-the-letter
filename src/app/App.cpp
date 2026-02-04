#include "App.h"

#include "../util/Json.h"

#include <algorithm>
#include <chrono>
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

app::app(app_config cfg,
         std::unique_ptr<mail_client> mail_client_ptr,
         std::unique_ptr<telegram_notifier> telegram_notifier_ptr,
         std::unique_ptr<storage> storage_ptr,
         std::unique_ptr<twilio_notifier> twilio_ptr)
  : cfg(std::move(cfg)),
    mail_client_ptr(std::move(mail_client_ptr)),
    telegram_notifier_ptr(std::move(telegram_notifier_ptr)),
    storage_ptr(std::move(storage_ptr)),
    twilio_ptr(std::move(twilio_ptr)) {}

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
  if (!mail_client_ptr || !storage_ptr) {
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

    int matched = 0;
    auto msgs = mail_client_ptr->fetch_unseen();
    for (const auto& msg : msgs) {
      if (storage_ptr->is_processed(msg.uid)) continue;

      auto res = engine.apply(msg, rules_copy);
      if (!res.matched) {
        storage_ptr->mark_processed(msg);
        continue;
      }
      matched++;

      for (const auto& a : res.actions) {
        bool ok = false;
        std::string err;
        int attempt = 0;
        int backoff = cfg.backoff_base_ms;
        while (attempt < cfg.max_retries) {
          if (send_action(msg, a, err)) {
            ok = true;
            break;
          }
          std::this_thread::sleep_for(milliseconds(backoff));
          backoff = std::min(backoff * 2, cfg.backoff_max_ms);
          attempt++;
        }

        notification_log log;
        log.uid = msg.uid;
        log.channel = a.channel;
        log.status = ok ? "ok" : "error";
        log.error = ok ? "" : err;
        log.ts_iso = now_iso();
        storage_ptr->log_notification(log);
        if (!ok) {
          std::lock_guard<std::mutex> lock(mu);
          status.last_error = err;
        }
      }

      storage_ptr->mark_processed(msg);
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      status.processed_total = storage_ptr->processed_count();
      status.matched_last = matched;
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
  j["processed_total"] = status.processed_total;
  j["matched_last"] = status.matched_last;
  return j.dump(2);
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
  return true;
}
