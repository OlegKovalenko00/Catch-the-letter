#pragma once

#include "../infra/MailClient.h"
#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"
#include "Config.h"

#include <string>
#include <vector>

struct attachment_fetch_result {
  bool ok = false;
  std::string local_path;
  std::string sha256;
  std::string error;
};


class attachment_service {
public:
  attachment_service(storage& store, telegram_bot& bot, const app_config& cfg)
    : store(store), bot(bot), cfg(cfg) {}


  void store_attachments(const std::string& email_id, const message& msg);


  attachment_fetch_result fetch_attachment(const std::string& attachment_id,
                                            mail_client& imap_client);


  bool send_to_telegram(const std::string& attachment_id, const std::string& caption,
                        std::string& err);

private:
  bool is_dangerous(const std::string& filename) const;
  bool is_too_large(std::size_t size_bytes, int max_mb) const;
  std::string ext_lower(const std::string& filename) const;

  storage& store;
  telegram_bot& bot;
  const app_config& cfg;
};
