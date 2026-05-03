#include "UserProfileLoader.h"

#include "../util/Json.h"

#include <fstream>

using nlohmann::json;

static void flatten_profile_json(const json& obj, const std::string& prefix, user_profile& out) {
  if (!obj.is_object()) return;
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
    if (it->is_object()) {
      flatten_profile_json(*it, key, out);
    } else if (it->is_string()) {
      out.values[key] = it->get<std::string>();
    } else if (it->is_number_integer()) {
      out.values[key] = std::to_string(it->get<long long>());
    } else if (it->is_number_unsigned()) {
      out.values[key] = std::to_string(it->get<unsigned long long>());
    } else if (it->is_number_float()) {
      out.values[key] = std::to_string(it->get<double>());
    } else if (it->is_boolean()) {
      out.values[key] = it->get<bool>() ? "true" : "false";
    }
  }
}

bool load_user_profile(const std::string& path, user_profile& out, std::string& err) {
  std::string text;
  if (!json_util::read_file(path, text, &err)) return false;

  json root;
  if (!json_util::parse(text, root, &err)) return false;
  out.values.clear();
  flatten_profile_json(root, "", out);
  return true;
}

std::string user_profile_to_json(const user_profile& profile) {
  json out;
  for (const auto& [key, value] : profile.values) {
    out[key] = value;
  }
  return out.dump(2);
}

bool save_user_profile(const std::string& path, const user_profile& profile, std::string& err) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot write profile file";
    return false;
  }
  f << user_profile_to_json(profile);
  return true;
}
