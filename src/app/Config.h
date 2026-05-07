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
  bool enabled = true;
  std::string bot_token_env = "TELEGRAM_BOT_TOKEN";
  std::string chat_id_env = "TELEGRAM_CHAT_ID";
  std::string proxy_url_env = "TELEGRAM_PROXY_URL";
  std::string bot_token;
  std::string chat_id;
  std::string proxy_url;
  bool poll_updates = true;
  int poll_interval_seconds = 2;
  bool captcha_remote_control_experimental = false;  // [experimental] screenshot+click in Telegram
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
  std::string web_public_base_url;  // for mobile Telegram; e.g. https://yourdomain.com
};

struct storage_config {
  std::string path = "data/app.db";
};

struct browser_worker_config {
  bool enabled = true;
  std::string endpoint = "http://127.0.0.1:8090";
  int timeout_seconds = 60;
};

struct form_providers_config {
  bool prefer_provider_api = true;
  bool browser_fallback_for_known_providers = false;
};

struct yandex_forms_api_config {
  bool enabled = true;
  std::string base_url = "https://api.forms.yandex.net/v1";
  std::string oauth_token_env = "YANDEX_FORMS_OAUTH_TOKEN";
  std::string org_id_env = "YANDEX_FORMS_ORG_ID";
  std::string cloud_org_id_env = "YANDEX_FORMS_CLOUD_ORG_ID";
  std::string oauth_token;
  std::string org_id;
  std::string cloud_org_id;
  std::string form_map_file = "config/yandex_forms.map.json";
  bool dry_run = false;
  bool allow_browser_fallback = false;
  int timeout_seconds = 60;
};

struct google_forms_api_config {
  bool enabled = true;
  std::string credentials_json_env = "GOOGLE_APPLICATION_CREDENTIALS";
  std::string oauth_token_env = "GOOGLE_FORMS_OAUTH_TOKEN";
  std::string credentials_json;
  std::string oauth_token;
  std::string form_map_file = "config/google_forms.map.json";
  bool dry_run = false;
  bool allow_browser_fallback = false;
  int timeout_seconds = 60;
};

struct llm_config {
  bool enabled = true;
  std::string provider = "ollama";
  std::string endpoint = "http://127.0.0.1:11434/api/chat";
  std::string endpoint_env = "LLM_ENDPOINT";
  std::string model = "qwen3:4b";
  std::string model_env = "LLM_MODEL";
  std::string privacy_mode = "safe";
  int timeout_seconds = 300;
  int healthcheck_timeout_seconds = 30;
  bool auto_fallback_to_noop = true;
  bool auto_pull = true;
  bool startup_probe = true;
  double min_memory_gb = 6.0;
  double recommended_memory_gb = 8.0;
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
  form_providers_config form_providers;
  yandex_forms_api_config yandex_forms_api;
  google_forms_api_config google_forms_api;
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
