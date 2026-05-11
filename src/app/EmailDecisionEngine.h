#pragma once

#include "../domain/EmailAnalysis.h"
#include "../domain/Message.h"
#include "Config.h"

#include <string>

enum class email_action {
  notify,
  form_fill,
  ignore,
};

struct email_decision {
  email_action action = email_action::ignore;
  std::string  reason;
};


class email_decision_engine {
public:
  explicit email_decision_engine(const mail_processing_config& cfg) : cfg(cfg) {}

  email_decision decide(const email_analysis& analysis, const message& msg) const;

private:
  bool meets_notify_threshold(const email_analysis& analysis) const;

  const mail_processing_config& cfg;
};
