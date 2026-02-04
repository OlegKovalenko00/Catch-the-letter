#include "app/App.h"
#include "app/Config.h"
#include "infra/HttpServer.h"
#include "infra/MailClient.h"
#include "infra/Storage.h"
#include "infra/TelegramNotifier.h"
#include "infra/TwilioNotifier.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

mail_client* make_mail_client_mock();
telegram_notifier* make_telegram_notifier_mock();

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
  a1.type = "notify";
  a1.channel = "console";
  a1.text = "Важное письмо: {{subject}}";
  r.actions.push_back(a1);

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
  bool once = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--demo") {
      demo = true;
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--help") {
      std::cout << "Usage: catch_the_letter --config <path> [--once]" << std::endl;
      return 0;
    }
  }

  if (demo) {
    app_config cfg;
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
    std::unique_ptr<mail_client> mail(make_mail_client_mock());
    std::unique_ptr<telegram_notifier> tg(make_telegram_notifier_mock());

    app application(cfg, std::move(mail), std::move(tg), std::move(store), nullptr);
    application.run(true);
    return 0;
  }

  app_config cfg;
  std::string err;
  if (!load_app_config(config_path, cfg, err)) {
    std::cerr << "config error: " << err << std::endl;
    return 1;
  }

  ensure_parent_dir(cfg.storage.path);

  std::unique_ptr<mail_client> mail(make_mail_client_imap(cfg.imap, &err));
  if (!mail) {
    std::cerr << "imap error: " << err << std::endl;
    return 1;
  }

  std::unique_ptr<telegram_notifier> tg;
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

  app application(cfg, std::move(mail), std::move(tg), std::move(store), std::move(twilio_ptr));

  std::unique_ptr<http_server> server;
  if (cfg.http.enabled) {
    http_handlers handlers;
    handlers.get_status_json = [&application]() { return application.status_json(); };
    handlers.get_rules_json = [&application]() { return application.rules_json(); };
    handlers.set_rules_json = [&application](const std::string& text, std::string& e) {
      return application.update_rules_json(text, e);
    };
    server = make_http_server(cfg.http, handlers, err);
    if (!server->start()) {
      std::cerr << "http server failed" << std::endl;
    }
  }

  application.run(once);

  if (server) server->stop();
  return 0;
}
