#pragma once

#include "Message.h"

#include <string>
#include <vector>

enum class message_kind {
  ignored,
  important_notification,
  form_request,
  auth_required,
  unknown
};

struct email_analysis {
  message_kind kind = message_kind::unknown;
  double confidence = 0.0;
  std::string summary;
  std::vector<message_link> form_links;
  bool user_action_required = false;
};
