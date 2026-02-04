#pragma once

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace json_util {

inline bool read_file(const std::string& path, std::string& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot открыть файл: " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

inline bool parse(const std::string& text, nlohmann::json& out, std::string* err) {
  try {
    out = nlohmann::json::parse(text);
    return true;
  } catch (const std::exception& e) {
    if (err) *err = std::string("ошибка JSON: ") + e.what();
    return false;
  }
}

} 
