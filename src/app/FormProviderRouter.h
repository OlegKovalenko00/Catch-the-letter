#pragma once

#include "Config.h"

#include "../domain/Form.h"

#include <nlohmann/json.hpp>

#include <string>

struct provider_route {
  form_provider_type provider_type = form_provider_type::generic_browser;
  form_submit_strategy submit_strategy = form_submit_strategy::browser_worker;
  std::string provider_name = "Generic Browser";
  bool known_provider = false;
  bool allow_browser_fallback = true;
};

struct provider_inspect_result {
  bool ok = false;
  bool manual_required = false;
  bool captcha_required = false;
  std::string provider = "";
  std::string extraction_strategy = "";
  std::string submit_strategy = "";
  std::string public_form_id = "";
  std::string api_form_id = "";
  std::string error = "";
  std::string debug_json = "{}";
  form_snapshot snapshot;
};

struct provider_submit_result {
  bool ok = false;
  bool submitted = false;
  std::string provider = "";
  std::string submit_strategy = "";
  std::string error = "";
  std::string debug_json = "{}";
};

class form_provider_router {
public:
  explicit form_provider_router(app_config cfg);

  provider_route route_for_url(const std::string& url) const;
  form_provider_type detect_provider(const std::string& url) const;

  static std::string provider_type_name(form_provider_type type);
  static std::string provider_display_name(form_provider_type type);
  static std::string submit_strategy_name(form_submit_strategy strategy);

private:
  app_config cfg;
};

