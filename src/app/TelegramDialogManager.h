#pragma once

#include "Config.h"
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

private:
  void loop();
  void handle_update(const telegram_update& update);
  void handle_callback(const telegram_update& update);
  void handle_text(const telegram_update& update);
  bool send_next_answer_prompt(const std::string& chat_id, const std::string& session_id, std::string& err);
  bool handle_option_answer(const std::string& chat_id, const std::string& session_id, const std::string& payload, std::string& err);
  static bool parse_field_line(const std::string& line, std::string& field_ref, std::string& value);

  telegram_config cfg;
  telegram_bot& bot;
  workflow_engine& workflow;
  storage& store;
  std::thread worker;
  std::atomic<bool> running{false};
};
