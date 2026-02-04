#pragma once

#include "../domain/Message.h"
#include "../app/Config.h"

#include <string>

class telegram_notifier {
public:
  virtual ~telegram_notifier() = default;
  virtual bool notify(const message& msg, const std::string& text, std::string& err) = 0;
};

telegram_notifier* make_telegram_notifier_http(const telegram_config& cfg, std::string* err);
