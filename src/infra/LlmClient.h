#pragma once

#include "../app/Config.h"
#include "../domain/EmailAnalysis.h"
#include "../domain/Form.h"
#include "../domain/UserProfile.h"

#include <memory>

class llm_client {
public:
  virtual ~llm_client() = default;
  virtual email_analysis analyze_email(const message& msg) = 0;
  virtual std::vector<form_field> map_fields(const message& msg,
                                             const form_snapshot& form,
                                             const user_profile& profile) = 0;
};

std::unique_ptr<llm_client> make_noop_llm_client();
std::unique_ptr<llm_client> make_ollama_client(const llm_config& cfg);
bool test_ollama_endpoint(const llm_config& cfg, std::string& err);
