#pragma once

#include "../app/Config.h"

#include <functional>
#include <memory>
#include <string>

struct http_handlers {
  std::function<std::string()> get_status_json;
  std::function<std::string()> get_dashboard_json;
  std::function<std::string(int)> get_events_json;
  std::function<std::string()> get_rules_json;
  std::function<bool(const std::string&, std::string&)> set_rules_json;
  std::function<std::string(bool)> get_active_forms_json;
  std::function<std::string(const std::string&)> get_form_json;
  std::function<bool(const std::string&, const std::string&, std::string&)> update_form_field_json;
  std::function<bool(const std::string&, std::string&)> fill_form;
  std::function<bool(const std::string&, std::string&)> submit_form;
  std::function<bool(const std::string&, std::string&)> manual_form;
  std::function<bool(const std::string&, std::string&)> cancel_form;
  std::function<bool(const std::string&, const std::string&, std::string&)> auth_credentials;
  std::function<bool(const std::string&, const std::string&, std::string&)> auth_two_factor;
  std::function<bool(const std::string&, std::string&)> reinspect_form;
  std::function<std::string()> get_profile_json;
  std::function<bool(const std::string&, std::string&)> set_profile_json;
  std::function<std::string()> get_config_json;
  std::function<std::string()> test_browser_json;
  std::function<std::string()> test_imap_json;
  std::function<std::string()> test_llm_json;
  std::function<std::string()> test_telegram_json;
  std::function<std::string(const std::string&)> inspect_form_url_json;
  std::function<std::string(const std::string&)> create_form_session_from_url_json;
  std::function<std::string(const std::string&, const std::string&)> remap_form_json;
  std::function<std::string(const std::string&, const std::string&)> explain_form_field_json;
  std::function<std::string(const std::string&)> validate_form_json;
  std::function<bool(bool, std::string&)> create_demo_form;
  std::function<std::string(const std::string&)> get_form_screenshot_png;
  std::function<bool(const std::string&, const std::string&, std::string&)> captcha_click_form;
  std::function<bool(const std::string&, std::string&)> captcha_reinspect_form;
  std::function<bool(std::string&)> create_demo_captcha_form;

  std::function<std::string()> mail_debug_json;
  std::function<std::string(int)> mail_scan_last_json;
  std::function<std::string(const std::string&)> mail_reset_state_json;

  std::function<std::string(const std::string&)> expand_profile_preview_json;
  std::function<bool(const std::string&, std::string&)> apply_profile_expansion_json;


  std::function<std::string(const std::string&, int, int)> mail_list_json;
  std::function<std::string(const std::string&)> mail_get_json;
  std::function<bool(const std::string&, std::string&)> mail_mark_read;
  std::function<bool(const std::string&, std::string&)> mail_archive;
  std::function<bool(const std::string&, const std::string&, std::string&)> mail_mute;
  std::function<std::string(const std::string&)> mail_attachments_json;
  std::function<bool(const std::string&, std::string&, std::string&, std::string&, std::string&)> mail_attachment_download;
  std::function<std::string(const std::string&, int, int)> mail_search_json;
};

class http_server {
public:
  virtual ~http_server() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
};

std::unique_ptr<http_server> make_http_server(const http_config& cfg,
                                              const http_handlers& handlers,
                                              std::string& err);
