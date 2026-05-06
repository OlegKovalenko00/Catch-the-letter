#pragma once

#include "../app/Config.h"
#include "../domain/EmailAnalysis.h"
#include "../domain/Form.h"
#include "../domain/UserProfile.h"

#include <memory>
#include <string>

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

struct ollama_probe_result {
  bool reachable = false;
  bool model_ready = false;
  long http_status = 0;
  int timeout_seconds = 0;
  int healthcheck_timeout_seconds = 0;
  double total_duration_ms = 0.0;
  std::string error;
};

ollama_probe_result probe_ollama_endpoint(const llm_config& cfg);
bool test_ollama_health(const llm_config& cfg, std::string& err);
bool test_ollama_endpoint(const llm_config& cfg, std::string& err);
