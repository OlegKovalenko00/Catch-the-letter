#include "BrowserWorkerClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <utility>

using nlohmann::json;

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string trim_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

form_field parse_field(const json& item) {
  form_field field;
  field.id = item.value("id", "");
  field.selector = item.value("selector", "");
  field.label = item.value("label", "");
  field.type = item.value("type", "unknown");
  field.required = item.value("required", false);
  if (item.contains("options") && item["options"].is_array()) {
    for (const auto& opt : item["options"]) {
      if (opt.is_string()) field.options.push_back(opt.get<std::string>());
    }
  }
  field.value = item.value("value", "");
  field.mapped_profile_key = item.value("mapped_profile_key", "");
  field.confidence = item.value("confidence", 0.0);
  field.requires_user_input = item.value("requires_user_input", false);
  return field;
}

}  // namespace

browser_worker_client::browser_worker_client(browser_worker_config cfg) : cfg(std::move(cfg)) {
  this->cfg.endpoint = trim_slash(this->cfg.endpoint);
}

bool browser_worker_client::get_json(const std::string& path, json& response, std::string& err) const {
  if (!cfg.enabled) {
    err = "browser-worker disabled";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string body;
  std::string url = cfg.endpoint + path;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = curl_easy_strerror(rc);
    return false;
  }
  if (code < 200 || code >= 300) {
    std::ostringstream ss;
    ss << "browser-worker HTTP " << code;
    err = ss.str();
    return false;
  }

  try {
    response = json::parse(body.empty() ? "{}" : body);
    err.clear();
    return true;
  } catch (const std::exception& e) {
    err = std::string("browser-worker JSON parse failed: ") + e.what();
    return false;
  }
}

bool browser_worker_client::post_json(const std::string& path,
                                      const json& request,
                                      json& response,
                                      std::string& err) const {
  if (!cfg.enabled) {
    err = "browser-worker disabled";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }

  std::string body;
  std::string payload = request.dump();
  std::string url = cfg.endpoint + path;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = curl_easy_strerror(rc);
    return false;
  }
  if (code < 200 || code >= 300) {
    std::ostringstream ss;
    ss << "browser-worker HTTP " << code;
    err = ss.str();
    return false;
  }

  try {
    response = json::parse(body.empty() ? "{}" : body);
  } catch (const std::exception& e) {
    err = std::string("browser-worker JSON parse failed: ") + e.what();
    return false;
  }

  if (!response.value("ok", true)) {
    err = response.value("error", "browser-worker returned ok=false");
    return false;
  }

  err.clear();
  return true;
}

bool browser_worker_client::health(std::string& err) const {
  json out;
  if (!get_json("/health", out, err)) return false;
  if (!out.value("ok", false)) {
    err = out.value("error", "browser-worker health failed");
    return false;
  }
  return true;
}

std::optional<form_snapshot> browser_worker_client::inspect_form(const std::string& url,
                                                                 std::string& err) const {
  json response;
  if (!post_json("/inspect-form", {{"url", url}, {"session_id", nullptr}}, response, err)) {
    return std::nullopt;
  }

  form_snapshot snapshot;
  snapshot.session_id = response.value("session_id", "");
  snapshot.url = response.value("url", url);
  snapshot.final_url = response.value("final_url", "");
  snapshot.title = response.value("title", "");
  snapshot.form_type = response.value("form_type", "unknown");
  snapshot.auth_required = response.value("auth_required", false);
  snapshot.screenshot_path = response.value("screenshot_path", "");
  if (response.contains("fields") && response["fields"].is_array()) {
    for (const auto& item : response["fields"]) snapshot.fields.push_back(parse_field(item));
  }
  return snapshot;
}

bool browser_worker_client::fill_form(const std::string& browser_session_id,
                                      const std::vector<form_field>& fields,
                                      std::string& err) const {
  json req;
  req["session_id"] = browser_session_id;
  req["fields"] = json::array();
  for (const auto& field : fields) {
    if (field.value.empty()) continue;
    req["fields"].push_back({
        {"id", field.id},
        {"selector", field.selector},
        {"value", field.value}
    });
  }

  json response;
  return post_json("/fill-form", req, response, err);
}

bool browser_worker_client::submit_form(const std::string& browser_session_id, std::string& err) const {
  auto result = submit_form_result(browser_session_id, err);
  return result.ok && result.submitted;
}

browser_submit_result browser_worker_client::submit_form_result(const std::string& browser_session_id,
                                                                std::string& err) const {
  browser_submit_result result;
  json response;
  if (!post_json("/submit-form", {{"session_id", browser_session_id}}, response, err)) {
    result.error = err;
    return result;
  }
  result.ok = true;
  result.submitted = response.value("submitted", false);
  result.needs_next = response.value("status", "") == "needs_next";
  result.error = response.value("error", "");
  if (response.contains("fields") && response["fields"].is_array()) {
    for (const auto& item : response["fields"]) result.fields.push_back(parse_field(item));
  }
  if (!result.submitted && !result.needs_next) {
    result.ok = false;
    if (result.error.empty()) result.error = "form was not submitted";
    err = result.error;
  } else {
    err.clear();
  }
  return result;
}

bool browser_worker_client::close_session(const std::string& browser_session_id, std::string& err) const {
  json response;
  return post_json("/close-session", {{"session_id", browser_session_id}}, response, err);
}

std::string browser_worker_client::enter_credentials(const std::string& browser_session_id,
                                                    const std::string& username,
                                                    const std::string& password,
                                                    std::string& err) const {
  json response;
  if (!post_json("/auth/credentials",
                 {{"session_id", browser_session_id}, {"username", username}, {"password", password}},
                 response,
                 err)) {
    return "failed";
  }
  return response.value("status", "failed");
}

std::string browser_worker_client::enter_two_factor_code(const std::string& browser_session_id,
                                                        const std::string& code,
                                                        std::string& err) const {
  json response;
  if (!post_json("/auth/2fa", {{"session_id", browser_session_id}, {"code", code}}, response, err)) {
    return "failed";
  }
  return response.value("status", "failed");
}

std::optional<form_snapshot> browser_worker_client::reinspect_form(const std::string& browser_session_id,
                                                                   std::string& err) const {
  json response;
  if (!post_json("/reinspect-form", {{"session_id", browser_session_id}}, response, err)) {
    return std::nullopt;
  }

  form_snapshot snapshot;
  snapshot.session_id = response.value("session_id", browser_session_id);
  snapshot.url = response.value("url", "");
  snapshot.final_url = response.value("final_url", "");
  snapshot.title = response.value("title", "");
  snapshot.form_type = response.value("form_type", "unknown");
  snapshot.auth_required = response.value("auth_required", false);
  snapshot.screenshot_path = response.value("screenshot_path", "");
  if (response.contains("fields") && response["fields"].is_array()) {
    for (const auto& item : response["fields"]) snapshot.fields.push_back(parse_field(item));
  }
  return snapshot;
}
