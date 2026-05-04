#pragma once

#include "../app/Config.h"

#include <functional>
#include <memory>
#include <string>

struct http_handlers {
  std::function<std::string()> get_status_json;
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
  std::function<std::string()> test_llm_json;
  std::function<std::string()> test_telegram_json;
  std::function<bool(bool, std::string&)> create_demo_form;
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
