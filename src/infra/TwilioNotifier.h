#pragma once

#include "../app/Config.h"
#include "../domain/Message.h"

#include <string>

class twilio_notifier {
public:
  explicit twilio_notifier(twilio_config cfg);

  bool send_sms(const message& msg, const std::string& text, std::string& err);
  bool make_call(const message& msg, const std::string& text, std::string& err);

private:
  twilio_config cfg;
};
