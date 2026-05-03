#pragma once

#include "../domain/UserProfile.h"

#include <string>

bool load_user_profile(const std::string& path, user_profile& out, std::string& err);
bool save_user_profile(const std::string& path, const user_profile& profile, std::string& err);
std::string user_profile_to_json(const user_profile& profile);
