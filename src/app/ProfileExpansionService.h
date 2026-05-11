#pragma once

#include "../app/Config.h"
#include "../domain/UserProfile.h"

#include <string>
#include <vector>

struct expansion_suggestion {
  std::string key;
  std::string value;
  std::string source;
  std::string reason;
  double confidence = 0.0;
};


bool is_expansion_key_allowed(const std::string& key);


std::vector<expansion_suggestion> suggest_profile_rules(const user_profile& profile);


std::vector<expansion_suggestion> suggest_profile_llm(const user_profile& profile,
                                                       const llm_config& cfg);


std::vector<expansion_suggestion> suggest_profile_expansions(const user_profile& profile,
                                                              const llm_config& cfg,
                                                              bool use_llm = true);
