#pragma once

#include "Config.h"
#include "TelegramMailController.h"
#include "WorkflowEngine.h"

#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"

#include <atomic>
#include <string>
#include <thread>

class telegram_dialog_manager {
public:
  telegram_dialog_manager(telegram_config cfg,
                          telegram_bot& bot,
                          workflow_engine& workflow,
                          storage& store);
  ~telegram_dialog_manager();

  void start();
  void stop();

  void set_mail_controller(telegram_mail_controller* controller) {
    mail_controller = controller;
  }

private:
  void loop();
  void handle_update(const telegram_update& update);
  void handle_callback(const telegram_update& update);
  void handle_text(const telegram_update& update);
  bool send_next_answer_prompt(const std::string& chat_id, const std::string& session_id, std::string& err);
  bool handle_option_answer(const std::string& chat_id, const std::string& session_id, const std::string& payload, std::string& err);
  static bool parse_field_line(const std::string& line, std::string& field_ref, std::string& value);

  void log_event(const std::string& level, const std::string& type,
                 const std::string& message, nlohmann::json data = nlohmann::json::object());

  telegram_config cfg;
  telegram_bot& bot;
  workflow_engine& workflow;
  storage& store;
  telegram_mail_controller* mail_controller = nullptr;
  std::thread worker;
  std::atomic<bool> running{false};
};
