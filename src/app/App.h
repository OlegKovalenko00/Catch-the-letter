#pragma once

#include "Config.h"

#include "../domain/RuleEngine.h"
#include "../infra/MailClient.h"
#include "../infra/Storage.h"
#include "../infra/TelegramNotifier.h"
#include "../infra/TwilioNotifier.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct app_status {
  std::string last_check;
  std::string last_error;
  int processed_total = 0;
  int matched_last = 0;
};

class app {
public:
  app(app_config cfg,
      std::unique_ptr<mail_client> mail_client_ptr,
      std::unique_ptr<telegram_notifier> telegram_notifier_ptr,
      std::unique_ptr<storage> storage_ptr,
      std::unique_ptr<twilio_notifier> twilio_ptr);

  void run(bool once);

  std::string status_json() const;
  std::string rules_json() const;
  bool update_rules_json(const std::string& text, std::string& err);

private:
  void load_rules_if_changed();
  bool send_action(const message& msg, const action& a, std::string& err);

  app_config cfg;
  std::unique_ptr<mail_client> mail_client_ptr;
  std::unique_ptr<telegram_notifier> telegram_notifier_ptr;
  std::unique_ptr<storage> storage_ptr;
  std::unique_ptr<twilio_notifier> twilio_ptr;
  rule_engine engine;

  mutable std::mutex mu;
  std::vector<rule> rules;
  std::string rules_raw;
  std::filesystem::file_time_type rules_mtime{};
  app_status status;
};
