#pragma once

#include "../domain/EmailAnalysis.h"
#include "../infra/Storage.h"
#include "../infra/TelegramBot.h"
#include "Config.h"

#include <string>


class notification_service {
public:
  notification_service(telegram_bot& bot, storage& store, const app_config& cfg)
    : bot(bot), store(store), cfg(cfg) {}

  bool notify_email(const stored_email& email, const email_analysis& analysis);

private:
  std::string format_message(const stored_email& email, const email_analysis& analysis) const;

  telegram_bot& bot;
  storage& store;
  const app_config& cfg;
};
