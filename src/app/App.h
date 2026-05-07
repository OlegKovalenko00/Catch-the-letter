#pragma once

#include "Config.h"
#include "EmailClassifier.h"
#include "TelegramDialogManager.h"
#include "WorkflowEngine.h"
#include "UserProfileLoader.h"

#include "../domain/RuleEngine.h"
#include "../domain/UserProfile.h"
#include "../infra/BrowserWorkerClient.h"
#include "../infra/LlmClient.h"
#include "../infra/MailClient.h"
#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"
#include "../infra/TelegramNotifier.h"
#include "../infra/TwilioNotifier.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct app_status {
  std::string last_check;
  std::string last_error;
  std::string mailbox_id;
  std::uint64_t last_seen_uid = 0;
  int processed_total = 0;
  int matched_last = 0;
};

struct mailbox_runtime {
  imap_config cfg;
  std::unique_ptr<mail_client> client;
  std::string last_check;
  std::string last_error;
  std::uint64_t last_seen_uid = 0;
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
  void start_async_services();
  void stop_async_services();

  std::string status_json() const;
  std::string events_json(int limit) const;
  std::string rules_json() const;
  bool update_rules_json(const std::string& text, std::string& err);
  std::string active_forms_json(bool all = false) const;
  std::string form_json(const std::string& id) const;
  bool update_form_field_json(const std::string& id, const std::string& body, std::string& err);
  bool fill_form(const std::string& id, std::string& err);
  bool submit_form(const std::string& id, std::string& err);
  bool mark_form_manual(const std::string& id, std::string& err);
  bool cancel_form(const std::string& id, std::string& err);
  bool auth_credentials(const std::string& id, const std::string& body, std::string& err);
  bool auth_two_factor(const std::string& id, const std::string& body, std::string& err);
  bool reinspect_form(const std::string& id, std::string& err);
  std::string profile_json() const;
  bool update_profile_json(const std::string& body, std::string& err);
  std::string config_json() const;
  std::string test_browser_json();
  std::string test_imap_json();
  std::string test_llm_json();
  std::string test_telegram_json();
  std::string inspect_form_url_json(const std::string& body);
  std::string create_form_session_from_url_json(const std::string& body);
  std::string remap_form_json(const std::string& id, const std::string& body = "{}");
  std::string explain_form_field_json(const std::string& id, const std::string& body);
  std::string validate_form_json(const std::string& id);
  bool create_demo_form(bool auth_demo, std::string& err);
  bool create_demo_captcha_form(std::string& err);
  std::string form_screenshot_png(const std::string& id);
  bool captcha_click_form(const std::string& id, const std::string& body, std::string& err);
  bool captcha_reinspect_form(const std::string& id, std::string& err);

private:
  mailbox_checkpoint ensure_checkpoint(mailbox_runtime& mailbox);
  void load_rules_if_changed();
  bool send_action(const message& msg, const action& a, std::string& err);
  void append_event(std::string level,
                    std::string type,
                    std::string message_text,
                    nlohmann::json data = nlohmann::json::object());

  app_config cfg;
  std::vector<mailbox_runtime> mailboxes;
  std::unique_ptr<telegram_notifier> telegram_notifier_ptr;
  std::unique_ptr<storage> storage_ptr;
  std::unique_ptr<twilio_notifier> twilio_ptr;
  rule_engine engine;
  user_profile profile;
  std::unique_ptr<telegram_bot> telegram_bot_ptr;
  std::unique_ptr<browser_worker_client> browser_ptr;
  std::unique_ptr<llm_client> llm_ptr;
  std::unique_ptr<email_classifier> classifier_ptr;
  std::unique_ptr<workflow_engine> workflow_ptr;
  std::unique_ptr<telegram_dialog_manager> dialog_manager_ptr;

  mutable std::mutex mu;
  std::vector<rule> rules;
  std::string rules_raw;
  std::filesystem::file_time_type rules_mtime{};
  app_status status;
};
