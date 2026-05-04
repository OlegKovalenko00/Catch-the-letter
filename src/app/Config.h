#pragma once

#include "../domain/Rule.h"

#include <string>
#include <vector>

struct imap_config {
  std::string mailbox_id = "main";
  std::string provider = "generic";
  std::string email;
  std::string auth_method = "password";
  std::string host;
  int port = 993;
  bool tls = true;
  std::string username;
  std::string password;
  std::string folder = "INBOX";
  std::string checkpoint_mode = "uid";
  int poll_interval_sec = 20;
  bool mark_seen = false;
};

struct telegram_config {
  bool enabled = false;
  std::string bot_token;
  std::string chat_id;
  bool poll_updates = false;
  int poll_interval_seconds = 2;
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
  std::string auth_token;
};

struct storage_config {
  std::string path = "data/app.db";
};

struct browser_worker_config {
  bool enabled = true;
  std::string endpoint = "http://127.0.0.1:8090";
  int timeout_seconds = 60;
};

struct llm_config {
  bool enabled = false;
  std::string provider = "ollama";
  std::string endpoint = "http://127.0.0.1:11434/api/chat";
  std::string model = "qwen3:8b";
  std::string privacy_mode = "safe";
  int timeout_seconds = 120;
};

struct security_config {
  std::string mode = "open";
  bool allow_private_networks = false;
  bool auto_submit = false;
  bool require_confirmation_before_submit = true;
  bool allow_password_via_telegram = false;
  std::vector<std::string> allowed_domains;
  std::vector<std::string> blocked_domains;
};

struct auth_config {
  bool enabled = true;
  bool allow_credentials_via_telegram = false;
  bool allow_credentials_via_web = true;
  bool remember_credentials = false;
  std::string credentials_storage = "memory";
  bool two_factor_via_telegram = true;
  bool two_factor_via_web = true;
};

struct app_config {
  imap_config imap;
  std::vector<imap_config> mailboxes;
  telegram_config telegram;
  twilio_config twilio;
  http_config http;
  storage_config storage;
  browser_worker_config browser_worker;
  llm_config llm;
  security_config security;
  auth_config auth;
  std::string profile_file = "config/profile.json";
  std::string rules_file = "config/rules.json";
  int max_retries = 3;
  int backoff_base_ms = 500;
  int backoff_max_ms = 8000;
  int events_limit = 200;
  std::string timezone = "Europe/Moscow";
  bool demo_mode = false;
  std::string log_level = "info";
};

bool load_app_config(const std::string& path, app_config& out, std::string& err);
bool load_rules(const std::string& path, std::vector<rule>& out, std::string& err);
std::string rules_to_json(const std::vector<rule>& rules);

std::string expand_env(const std::string& input);
