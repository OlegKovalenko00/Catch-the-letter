#pragma once

#include "../app/Config.h"
#include "../domain/Form.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

struct browser_submit_result {
  bool ok = false;
  bool submitted = false;
  bool needs_next = false;
  std::vector<form_field> fields;
  std::string error;
};

class browser_worker_client {
public:
  explicit browser_worker_client(browser_worker_config cfg);

  bool health(std::string& err) const;
  std::optional<form_snapshot> inspect_form(const std::string& url, std::string& err) const;
  bool fill_form(const std::string& browser_session_id,
                 const std::vector<form_field>& fields,
                 std::string& err) const;
  bool submit_form(const std::string& browser_session_id, std::string& err) const;
  browser_submit_result submit_form_result(const std::string& browser_session_id,
                                           std::string& err) const;
  bool close_session(const std::string& browser_session_id, std::string& err) const;
  std::string enter_credentials(const std::string& browser_session_id,
                                const std::string& username,
                                const std::string& password,
                                std::string& err) const;
  std::string enter_two_factor_code(const std::string& browser_session_id,
                                    const std::string& code,
                                    std::string& err) const;
  std::optional<form_snapshot> reinspect_form(const std::string& browser_session_id,
                                              std::string& err) const;

private:
  bool post_json(const std::string& path,
                 const nlohmann::json& request,
                 nlohmann::json& response,
                 std::string& err) const;
  bool get_json(const std::string& path, nlohmann::json& response, std::string& err) const;

  browser_worker_config cfg;
};
