#pragma once

#include <string>
#include <vector>

struct message {
  std::string uid;
  std::string message_id;
  std::string from;
  std::string to;
  std::string subject;
  std::string snippet;
  std::string body;
  std::string date_iso;
  std::vector<std::string> labels;
};
