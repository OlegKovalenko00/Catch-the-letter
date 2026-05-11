#pragma once

#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"
#include "Config.h"

#include <string>


class telegram_mail_controller {
public:
  telegram_mail_controller(telegram_bot& bot, storage& store, const app_config& cfg)
    : bot(bot), store(store), cfg(cfg) {}


  bool handle_command(const std::string& chat_id, const std::string& text);


  bool handle_callback(const std::string& chat_id,
                       const std::string& callback_query_id,
                       const std::string& callback_data);

private:
  static const int PAGE_BODY_CHARS = 3000;

  void cmd_mail(const std::string& chat_id);
  void cmd_important(const std::string& chat_id);
  void cmd_unread(const std::string& chat_id);
  void cmd_digest(const std::string& chat_id);
  void cmd_search(const std::string& chat_id, const std::string& query);
  void cmd_diagnostics(const std::string& chat_id);

  void send_email_list(const std::string& chat_id,
                       const std::vector<stored_email>& emails,
                       const std::string& title);
  void send_email_detail(const std::string& chat_id,
                         const stored_email& email,
                         int page = 0);
  void send_email_attachments(const std::string& chat_id, const stored_email& email);

  void handle_mtok(const std::string& chat_id,
                   const std::string& callback_query_id,
                   const std::string& token);

  telegram_bot& bot;
  storage& store;
  const app_config& cfg;
};
