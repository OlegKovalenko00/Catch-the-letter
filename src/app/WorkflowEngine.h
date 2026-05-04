#pragma once

#include "Config.h"
#include "EmailClassifier.h"

#include "../domain/RuleEngine.h"
#include "../domain/UserProfile.h"
#include "../infra/BrowserWorkerClient.h"
#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"
#include "../infra/UrlPolicy.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

struct workflow_result {
  bool matched = false;
  std::string status;
};

class workflow_engine {
public:
  workflow_engine(app_config cfg,
                  storage& store,
                  rule_engine& rules,
                  email_classifier& classifier,
                  browser_worker_client& browser,
                  llm_client& llm,
                  user_profile profile,
                  telegram_bot* telegram);

  workflow_result handle_message(const message& msg, const std::vector<rule>& rules);
  bool fill_form_after_review(const std::string& session_id, std::string& err);
  bool submit_form_after_confirm(const std::string& session_id, std::string& err);
  bool mark_manual_required(const std::string& session_id, std::string& err);
  bool cancel_form(const std::string& session_id, std::string& err);
  bool submit_auth_credentials(const std::string& session_id,
                               const std::string& username,
                               const std::string& password,
                               std::string& err);
  bool submit_two_factor_code(const std::string& session_id,
                              const std::string& code,
                              std::string& err);
  bool reinspect_after_auth(const std::string& session_id, std::string& err);
  bool update_field_value(const std::string& session_id,
                          const std::string& field_ref,
                          const std::string& value,
                          std::string& err);
  bool send_form_review(const form_session& session, std::string& err);
  bool send_submit_confirmation(const form_session& session, std::string& err);
  bool notify_text(const std::string& text, std::string& err);
  bool test_browser(std::string& err);
  bool test_llm(std::string& err);
  bool test_telegram(std::string& err);
  void set_profile(user_profile next_profile);
  bool create_demo_session(const std::string& url,
                           const std::string& title,
                           bool auth_demo,
                           std::string& err);

private:
  void append_event(std::string level,
                    std::string type,
                    std::string message_text,
                    nlohmann::json data = nlohmann::json::object());
  bool start_form_workflow(const message& msg, const email_analysis& analysis, std::string& status);
  void notify_important(const message& msg, const email_analysis& analysis);
  void notify_manual(const std::string& text);
  std::optional<link> choose_form_link(const message& msg, const email_analysis& analysis) const;

  app_config cfg;
  storage& store;
  rule_engine& rules_engine;
  email_classifier& classifier;
  browser_worker_client& browser;
  llm_client& llm;
  user_profile profile;
  telegram_bot* telegram = nullptr;
};
