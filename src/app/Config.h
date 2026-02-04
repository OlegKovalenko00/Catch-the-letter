#pragma once

#include "../domain/Rule.h"

#include <string>
#include <vector>

struct imap_config {
  std::string host;
  int port = 993;
  bool tls = true;
  std::string username;
  std::string password;
  std::string folder = "INBOX";
  int poll_interval_sec = 20;
  bool mark_seen = false;
};

struct telegram_config {
  bool enabled = false;
  std::string bot_token;
  std::string chat_id;
};

struct twilio_config {
  bool enabled = false;
  std::string account_sid;
  std::string auth_token;
  std::string from_number;
  std::string sms_to;
  std::string voice_to;
};

struct http_config {
  bool enabled = true;
  std::string host = "127.0.0.1";
  int port = 8080;
};

struct storage_config {
  std::string path = "data/app.db";
};

struct app_config {
  imap_config imap;
  telegram_config telegram;
  twilio_config twilio;
  http_config http;
  storage_config storage;
  std::string rules_file = "config/rules.json";
  int max_retries = 3;
  int backoff_base_ms = 500;
  int backoff_max_ms = 8000;
};

bool load_app_config(const std::string& path, app_config& out, std::string& err);
bool load_rules(const std::string& path, std::vector<rule>& out, std::string& err);
std::string rules_to_json(const std::vector<rule>& rules);

std::string expand_env(const std::string& input);
