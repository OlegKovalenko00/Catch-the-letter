#pragma once

#include "../app/Config.h"

#include <string>

bool is_allowed_url(const std::string& url, const security_config& cfg, std::string& reason);
std::string sanitize_url_for_log(const std::string& url);
