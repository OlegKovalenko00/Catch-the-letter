#include "app/App.h"
#include "app/Config.h"
#include "infra/HttpServer.h"
#include "infra/BrowserWorkerClient.h"
#include "infra/LlmClient.h"
#include "infra/MailClient.h"
#include "infra/Storage.h"
#include "infra/TelegramBot.h"
#include "infra/TelegramNotifier.h"
#include "infra/TwilioNotifier.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

mail_client* make_mail_client_mock();
telegram_notifier* make_telegram_notifier_mock();

class synthetic_mail_client final : public mail_client {
public:
  explicit synthetic_mail_client(message msg) : msg(std::move(msg)) {}

  std::uint64_t fetch_max_uid() override {
    return 0;
  }

  std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) override {
    if (last_seen_uid >= 1) return {};
    return {msg};
  }

private:
  message msg;
};

static std::string host_from_url(const std::string& url) {
  auto scheme = url.find("://");
  size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  size_t end = url.find('/', start);
  std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  auto colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  return host;
}

static message make_demo_message(const std::string& url, const std::string& subject) {
  message msg;
  msg.uid = "1";
  msg.mailbox_id = "demo";
  msg.provider = "demo";
  msg.message_id = "<demo@catch-the-letter>";
  msg.from = "dean@university.edu";
  msg.to = "student@example.com";
  msg.subject = subject;
  msg.snippet = "Please fill this form.";
  msg.body = "Please fill this form: " + url;
  msg.body_text = msg.body;
  msg.date_iso = "2026-05-03";
  msg.links.push_back({url, host_from_url(url), 0.95});
  return msg;
}

static rule make_rule_university_important() {
  rule r;
  r.id = "r1";
  r.name = "university important";
  r.priority = "important";
  r.match = match_mode::all;

  condition c1;
  c1.field = "from";
  c1.op = cond_op::contains;
  c1.value = "@university";
  r.conditions.push_back(c1);

  condition c2;
  c2.field = "subject";
  c2.op = cond_op::contains;
  c2.value = "Important";
  r.conditions.push_back(c2);

  action a1;
  a1.type = "classify";
  r.actions.push_back(a1);

  action a2;
  a2.type = "detect_form";
  r.actions.push_back(a2);

  action a3;
  a3.type = "notify";
  a3.channel = "console";
  a3.text = "Важное письмо: {{subject}}";
  r.actions.push_back(a3);

  return r;
}

static void ensure_parent_dir(const std::string& path) {
  std::error_code ec;
  std::filesystem::path p(path);
  auto parent = p.parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
}

int main(int argc, char** argv) {
  std::string config_path = "config/app.json";
  bool demo = false;
  bool demo_auth = false;
  bool once = false;
  bool test_config = false;
  bool test_browser = false;
  bool test_llm = false;
  bool test_telegram = false;
  int events_limit_override = 0;
  std::string log_level_override;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--events-limit" && i + 1 < argc) {
      try {
        events_limit_override = std::stoi(argv[++i]);
      } catch (...) {
        std::cerr << "invalid --events-limit" << std::endl;
        return 1;
      }
    } else if (arg == "--log-level" && i + 1 < argc) {
      log_level_override = argv[++i];
    } else if (arg == "--demo") {
      demo = true;
    } else if (arg == "--demo-auth") {
      demo = true;
      demo_auth = true;
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--test-config") {
      test_config = true;
    } else if (arg == "--test-browser") {
      test_browser = true;
    } else if (arg == "--test-llm") {
      test_llm = true;
    } else if (arg == "--test-telegram") {
      test_telegram = true;
    } else if (arg == "--help") {
      std::cout << "Usage: catch_the_letter --config <path> [--once] [--demo] [--demo-auth] "
                   "[--test-config] [--test-browser] [--test-llm] [--test-telegram] "
                   "[--events-limit N] [--log-level LEVEL]"
                << std::endl;
      return 0;
    }
  }

  if (demo) {
    app_config cfg;
    if (events_limit_override > 0) cfg.events_limit = events_limit_override;
    if (!log_level_override.empty()) cfg.log_level = log_level_override;
    const char* endpoint_env = std::getenv("BROWSER_WORKER_ENDPOINT");
    cfg.browser_worker.endpoint = endpoint_env ? endpoint_env : "http://127.0.0.1:8090";
    cfg.security.allow_private_networks = true;
    cfg.rules_file = "config/rules.demo.json";
    std::vector<rule> demo_rules{make_rule_university_important()};
    std::string demo_json = rules_to_json(demo_rules);
    ensure_parent_dir(cfg.rules_file);
    std::ofstream rf(cfg.rules_file, std::ios::binary);
    rf << demo_json;
    rf.close();

    std::string err;
    ensure_parent_dir(cfg.storage.path);
    std::unique_ptr<storage> store(make_sqlite_storage(cfg.storage.path, &err));
    std::string url = cfg.browser_worker.endpoint + (demo_auth ? "/demo-auth-form" : "/demo-form");
    std::string subject = demo_auth ? "Important: please fill auth form" : "Important: please fill form";
    std::unique_ptr<mail_client> mail(new synthetic_mail_client(make_demo_message(url, subject)));
    std::unique_ptr<telegram_notifier> tg(make_telegram_notifier_mock());

    app application(cfg, std::move(mail), std::move(tg), std::move(store), nullptr);
    application.run(true);
    return 0;
  }

  app_config cfg;
  std::string err;
  if (!std::filesystem::exists(config_path) &&
      std::filesystem::path(config_path).filename() == "app.json") {
    auto fallback = std::filesystem::path(config_path).parent_path() / "app.example.json";
    if (std::filesystem::exists(fallback)) config_path = fallback.string();
  }
  if (!load_app_config(config_path, cfg, err)) {
    std::cerr << "config error: " << err << std::endl;
    return 1;
  }
  if (events_limit_override > 0) cfg.events_limit = events_limit_override;
  if (!log_level_override.empty()) cfg.log_level = log_level_override;

  if (test_config) {
    std::cout << "config ok" << std::endl;
    return 0;
  }

  if (test_browser) {
    browser_worker_client browser(cfg.browser_worker);
    if (browser.health(err)) {
      std::cout << "browser-worker ok" << std::endl;
      return 0;
    }
    std::cerr << "browser-worker error: " << err << std::endl;
    return 1;
  }

  if (test_llm) {
    std::unique_ptr<llm_client> llm = cfg.llm.enabled ? make_ollama_client(cfg.llm) : make_noop_llm_client();
    message msg;
    msg.subject = "Important form";
    msg.body_text = "Please fill this form";
    auto analysis = llm->analyze_email(msg);
    std::cout << "llm test kind=" << static_cast<int>(analysis.kind) << std::endl;
    return 0;
  }

  if (test_telegram) {
    telegram_bot bot(cfg.telegram);
    if (!bot.enabled()) {
      std::cerr << "telegram disabled or incomplete" << std::endl;
      return 1;
    }
    if (bot.send_message("Catch the Letter: test message", err)) {
      std::cout << "telegram ok" << std::endl;
      return 0;
    }
    std::cerr << "telegram error: " << err << std::endl;
    return 1;
  }

  ensure_parent_dir(cfg.storage.path);

  std::unique_ptr<telegram_notifier> tg;
  if (cfg.telegram.enabled && (cfg.telegram.bot_token.empty() || cfg.telegram.chat_id.empty())) {
    std::cerr << "telegram disabled: token or chat_id is missing" << std::endl;
    cfg.telegram.enabled = false;
  }
  if (cfg.telegram.enabled) {
    tg.reset(make_telegram_notifier_http(cfg.telegram, &err));
    if (!tg) {
      std::cerr << "telegram error: " << err << std::endl;
      return 1;
    }
  }

  std::unique_ptr<twilio_notifier> twilio_ptr;
  if (cfg.twilio.enabled) {
    twilio_ptr = std::make_unique<twilio_notifier>(cfg.twilio);
  }

  std::unique_ptr<storage> store(make_sqlite_storage(cfg.storage.path, &err));
  if (!store) {
    std::cerr << "storage error: " << err << std::endl;
    return 1;
  }

  app application(cfg, std::unique_ptr<mail_client>{}, std::move(tg), std::move(store), std::move(twilio_ptr));

  std::unique_ptr<http_server> server;
  if (cfg.http.enabled) {
    http_handlers handlers;
    handlers.get_status_json = [&application]() { return application.status_json(); };
    handlers.get_events_json = [&application](int limit) { return application.events_json(limit); };
    handlers.get_rules_json = [&application]() { return application.rules_json(); };
    handlers.set_rules_json = [&application](const std::string& text, std::string& e) {
      return application.update_rules_json(text, e);
    };
    handlers.get_active_forms_json = [&application](bool all) { return application.active_forms_json(all); };
    handlers.get_form_json = [&application](const std::string& id) { return application.form_json(id); };
    handlers.update_form_field_json = [&application](const std::string& id, const std::string& body, std::string& e) {
      return application.update_form_field_json(id, body, e);
    };
    handlers.fill_form = [&application](const std::string& id, std::string& e) {
      return application.fill_form(id, e);
    };
    handlers.submit_form = [&application](const std::string& id, std::string& e) {
      return application.submit_form(id, e);
    };
    handlers.manual_form = [&application](const std::string& id, std::string& e) {
      return application.mark_form_manual(id, e);
    };
    handlers.cancel_form = [&application](const std::string& id, std::string& e) {
      return application.cancel_form(id, e);
    };
    handlers.auth_credentials = [&application](const std::string& id, const std::string& body, std::string& e) {
      return application.auth_credentials(id, body, e);
    };
    handlers.auth_two_factor = [&application](const std::string& id, const std::string& body, std::string& e) {
      return application.auth_two_factor(id, body, e);
    };
    handlers.reinspect_form = [&application](const std::string& id, std::string& e) {
      return application.reinspect_form(id, e);
    };
    handlers.get_profile_json = [&application]() { return application.profile_json(); };
    handlers.set_profile_json = [&application](const std::string& body, std::string& e) {
      return application.update_profile_json(body, e);
    };
    handlers.get_config_json = [&application]() { return application.config_json(); };
    handlers.test_browser_json = [&application]() { return application.test_browser_json(); };
    handlers.test_llm_json = [&application]() { return application.test_llm_json(); };
    handlers.test_telegram_json = [&application]() { return application.test_telegram_json(); };
    handlers.create_demo_form = [&application](bool auth_demo, std::string& e) {
      return application.create_demo_form(auth_demo, e);
    };
    server = make_http_server(cfg.http, handlers, err);
    if (!server->start()) {
      std::cerr << "http server failed" << std::endl;
    }
  }

  if (!once) application.start_async_services();
  application.run(once);
  application.stop_async_services();

  if (server) server->stop();
  return 0;
}
