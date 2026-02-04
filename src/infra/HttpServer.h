#pragma once

#include "../app/Config.h"

#include <functional>
#include <memory>
#include <string>

struct http_handlers {
  std::function<std::string()> get_status_json;
  std::function<std::string()> get_rules_json;
  std::function<bool(const std::string&, std::string&)> set_rules_json;
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
