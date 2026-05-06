#pragma once

#include <string>
#include <vector>

struct message_link {
  std::string url;
  std::string domain;
  double confidence = 0.0;
};

struct attachment {
  std::string filename;
  std::string mime_type;
  std::size_t size_bytes = 0;
};

struct message {
  std::string mailbox_id = "default";
  std::string provider;
  std::string uid;
  std::string message_id;
  std::string from;
  std::string to;
  std::string subject;
  std::string snippet;
  std::string body;
  std::string body_text;
  std::string body_html;
  std::string date_iso;
  std::vector<std::string> labels;
  std::vector<message_link> links;
  std::vector<attachment> attachments;
};
