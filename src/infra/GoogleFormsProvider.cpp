#include "GoogleFormsProvider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

using nlohmann::json;

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string lower_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool read_json_file(const std::string& path, json& out) {
  if (path.empty()) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  try {
    input >> out;
    return out.is_object();
  } catch (...) {
    return false;
  }
}

std::string json_string(const json& obj, const std::string& key, const std::string& def = "") {
  if (!obj.contains(key)) return def;
  const auto& value = obj.at(key);
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());
  if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
  return def;
}

field_option option_from_json(const json& item) {
  field_option option;
  if (item.is_string()) {
    option.label = item.get<std::string>();
    option.value = option.label;
    option.id = option.value;
    return option;
  }
  if (!item.is_object()) return option;
  option.label = json_string(item, "label");
  option.value = json_string(item, "value", option.label);
  option.id = json_string(item, "id", option.value);
  option.selector = json_string(item, "selector");
  return option;
}

form_field field_from_mapping(const json& item, const std::string& strategy) {
  form_field field;
  field.id = json_string(item, "id", json_string(item, "api_question_id"));
  field.api_question_id = json_string(item, "api_question_id", field.id);
  field.label = json_string(item, "label", field.api_question_id);
  field.normalized_label = json_string(item, "normalized_label");
  field.type = json_string(item, "type", "text");
  field.required = item.value("required", false);
  field.api_answer_type = json_string(item, "api_answer_type", field.type);
  field.provider = "google_forms";
  field.submit_strategy = strategy;
  field.semantic_key_hint = json_string(item, "semantic_key_hint");
  field.semantic_key = field.semantic_key_hint;
  field.virtual_field = item.value("virtual_field", true);
  field.diagnostic_only = item.value("diagnostic_only", false);
  if (item.contains("options") && item["options"].is_array()) {
    for (const auto& opt : item["options"]) {
      auto option = option_from_json(opt);
      if (!option.label.empty() || !option.value.empty() || !option.id.empty()) {
        field.options.push_back(option);
        if (!option.id.empty()) field.api_option_ids.push_back(option.id);
      }
    }
  }
  return field;
}

std::string strategy_from_submit_mode(const std::string& mode) {
  const std::string lower = lower_ascii(mode);
  if (lower == "response_endpoint" || lower == "form_response") return "google_forms_response_endpoint";
  if (lower == "manual") return "manual";
  return "google_forms_api";
}

json build_payload(const form_session& session) {
  json answers = json::object();
  for (const auto& field : session.fields) {
    if (field.diagnostic_only) continue;
    std::string question_id = field.api_question_id.empty() ? field.id : field.api_question_id;
    if (question_id.empty()) continue;
    if (!field.values.empty()) answers[question_id] = field.values;
    else answers[question_id] = field.value;
  }
  json payload = {
      {"provider", "google_forms"},
      {"strategy", session.submit_strategy},
      {"public_form_id", session.public_form_id},
      {"api_form_id", session.api_form_id.empty() ? session.public_form_id : session.api_form_id},
      {"answers", answers}
  };
  try {
    json debug = json::parse(session.provider_debug_json.empty() ? "{}" : session.provider_debug_json);
    if (debug.contains("form_response_url")) payload["form_response_url"] = debug["form_response_url"];
  } catch (...) {
  }
  return payload;
}

bool curl_request(const std::string& url,
                  const std::string& method,
                  const std::string& body,
                  const std::vector<std::string>& headers_in,
                  int timeout,
                  long& status,
                  std::string& response,
                  std::string& err) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl init failed";
    return false;
  }
  struct curl_slist* headers = nullptr;
  for (const auto& header : headers_in) headers = curl_slist_append(headers, header.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    err = curl_easy_strerror(rc);
    return false;
  }
  return true;
}

std::string url_encode(const std::string& value) {
  CURL* curl = curl_easy_init();
  if (!curl) return value;
  char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  std::string out = encoded ? encoded : value;
  if (encoded) curl_free(encoded);
  curl_easy_cleanup(curl);
  return out;
}

std::string build_form_response_body(const form_session& session) {
  std::vector<std::string> parts;
  for (const auto& field : session.fields) {
    if (field.diagnostic_only) continue;
    std::string key = field.api_question_id.empty() ? field.id : field.api_question_id;
    if (key.empty()) continue;
    if (!field.values.empty()) {
      for (const auto& value : field.values) {
        parts.push_back(url_encode(key) + "=" + url_encode(value));
      }
    } else {
      parts.push_back(url_encode(key) + "=" + url_encode(field.value));
    }
  }
  std::ostringstream ss;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) ss << "&";
    ss << parts[i];
  }
  return ss.str();
}

std::vector<form_field> parse_google_items(const json& root) {
  std::vector<form_field> fields;
  if (!root.contains("items") || !root["items"].is_array()) return fields;
  for (const auto& item : root["items"]) {
    if (!item.is_object()) continue;
    const json question = item.value("questionItem", json::object()).value("question", json::object());
    if (!question.is_object()) continue;
    form_field field;
    field.api_question_id = json_string(question, "questionId", json_string(item, "itemId"));
    field.id = field.api_question_id;
    field.label = json_string(item, "title", field.id);
    field.required = question.value("required", false);
    field.type = "text";
    field.provider = "google_forms";
    field.submit_strategy = "google_forms_api";
    field.virtual_field = true;
    if (question.contains("choiceQuestion")) {
      const auto& choice = question["choiceQuestion"];
      std::string choice_type = json_string(choice, "type", "RADIO");
      field.type = lower_ascii(choice_type) == "checkbox" ? "checkbox_group" : "radio_group";
      if (choice.contains("options") && choice["options"].is_array()) {
        for (const auto& opt : choice["options"]) {
          field_option option;
          option.label = json_string(opt, "value");
          option.value = option.label;
          option.id = option.value;
          if (!option.label.empty()) field.options.push_back(option);
        }
      }
    }
    if (!field.id.empty()) fields.push_back(std::move(field));
  }
  return fields;
}

std::string debug_value(const form_session& session, const std::string& key) {
  try {
    json debug = json::parse(session.provider_debug_json.empty() ? "{}" : session.provider_debug_json);
    return json_string(debug, key);
  } catch (...) {
    return "";
  }
}

}  // namespace

google_forms_provider::google_forms_provider(google_forms_api_config cfg) : cfg(std::move(cfg)) {}

bool google_forms_provider::can_handle(const std::string& url) const {
  const std::string lower = lower_ascii(url);
  return lower.find("docs.google.com/forms") != std::string::npos ||
         lower.find("forms.gle") != std::string::npos ||
         lower.find("google.com/forms") != std::string::npos ||
         lower.find("/forms/d/e/") != std::string::npos ||
         lower.find("/forms/d/") != std::string::npos;
}

std::string google_forms_provider::extract_google_form_id(const std::string& url) const {
  const std::string lower = lower_ascii(url);
  for (const std::string marker : {"/forms/d/e/", "/forms/d/"}) {
    std::size_t pos = lower.find(marker);
    if (pos == std::string::npos) continue;
    std::size_t start = pos + marker.size();
    std::size_t end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  }
  std::size_t scheme = lower.find("://");
  std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  std::size_t slash = lower.find('/', start);
  if (lower.find("forms.gle", start) == start && slash != std::string::npos) {
    std::size_t end = url.find_first_of("?#", slash + 1);
    return url.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1);
  }
  return "";
}

provider_inspect_result google_forms_provider::inspect(const std::string& url) const {
  provider_inspect_result result;
  result.provider = "google_forms";
  result.submit_strategy = "google_forms_api";
  result.snapshot.url = url;
  result.snapshot.final_url = url;
  result.snapshot.form_type = "google_forms";
  result.public_form_id = extract_google_form_id(url);

  if (result.public_form_id.empty()) {
    result.manual_required = true;
    result.error = "Google Forms provider could not extract a form id from this URL. Use a mapping file entry for the public id or open manually.";
    result.debug_json = json({{"url", url}, {"provider", result.provider}}).dump();
    return result;
  }

  json mapping_root;
  if (read_json_file(cfg.form_map_file, mapping_root) && mapping_root.contains(result.public_form_id)) {
    const auto& entry = mapping_root[result.public_form_id];
    result.ok = true;
    result.extraction_strategy = "google_forms_mapping";
    result.api_form_id = json_string(entry, "api_form_id", result.public_form_id);
    std::string strategy = strategy_from_submit_mode(json_string(entry, "submit_mode", "api"));
    result.submit_strategy = strategy;
    result.snapshot.title = json_string(entry, "title", "Google Form");
    if (entry.contains("questions") && entry["questions"].is_array()) {
      for (const auto& question : entry["questions"]) {
        result.snapshot.fields.push_back(field_from_mapping(question, strategy));
      }
    }
    result.debug_json = json({
        {"mapping_file", cfg.form_map_file},
        {"public_form_id", result.public_form_id},
        {"api_form_id", result.api_form_id},
        {"form_response_url", json_string(entry, "form_response_url")},
        {"field_count", result.snapshot.fields.size()}
    }).dump();
    return result;
  }

  if (cfg.enabled && !cfg.oauth_token.empty()) {
    const std::string endpoint = "https://forms.googleapis.com/v1/forms/" + result.public_form_id;
    std::vector<std::string> headers = {
        "Accept: application/json",
        "Authorization: Bearer " + cfg.oauth_token
    };
    long status = 0;
    std::string body;
    std::string err;
    if (curl_request(endpoint, "GET", "", headers, cfg.timeout_seconds, status, body, err) &&
        status >= 200 && status < 300) {
      try {
        json root = json::parse(body.empty() ? "{}" : body);
        result.ok = true;
        result.extraction_strategy = "google_forms_api";
        result.api_form_id = json_string(root, "formId", result.public_form_id);
        result.snapshot.title = json_string(root.value("info", json::object()), "title", "Google Form");
        result.snapshot.fields = parse_google_items(root);
        result.debug_json = json({{"metadata_endpoint", endpoint}, {"http_status", status}}).dump();
        return result;
      } catch (const std::exception& e) {
        result.error = std::string("Google Forms API metadata parse failed: ") + e.what();
      }
    } else {
      result.error = err.empty() ? "Google Forms API metadata request failed" : err;
      result.debug_json = json({{"metadata_endpoint", endpoint}, {"http_status", status}, {"response", body}}).dump();
    }
  }

  result.manual_required = true;
  result.error = "Google Forms provider requires API credentials or mapping file.";
  result.debug_json = json({
      {"mapping_file", cfg.form_map_file},
      {"api_enabled", cfg.enabled},
      {"oauth_configured", !cfg.oauth_token.empty()},
      {"credentials_json_configured", !cfg.credentials_json.empty()}
  }).dump();
  return result;
}

provider_submit_result google_forms_provider::submit(const form_session& session) const {
  provider_submit_result result;
  result.provider = "google_forms";
  result.submit_strategy = session.submit_strategy.empty() ? "google_forms_api" : session.submit_strategy;
  json payload = build_payload(session);
  result.debug_json = json({{"payload_preview", payload}, {"dry_run", cfg.dry_run}}).dump();

  if (cfg.dry_run) {
    result.ok = true;
    result.submitted = true;
    return result;
  }
  if (result.submit_strategy == "manual") {
    result.error = "Google Forms submit strategy is manual.";
    return result;
  }
  for (const auto& field : session.fields) {
    if (field.diagnostic_only) continue;
    if (field.api_question_id.empty() && field.id.empty()) {
      result.error = "Google Forms provider submit requires api_question_id or entry id for every mapped field.";
      return result;
    }
  }
  if (result.submit_strategy == "google_forms_response_endpoint") {
    std::string endpoint = debug_value(session, "form_response_url");
    if (endpoint.empty()) {
      result.error = "Google Forms response endpoint submit requires form_response_url in mapping file.";
      return result;
    }
    std::string response;
    std::string err;
    long status = 0;
    std::string body = build_form_response_body(session);
    std::vector<std::string> headers = {"Content-Type: application/x-www-form-urlencoded"};
    bool ok = curl_request(endpoint, "POST", body, headers, cfg.timeout_seconds, status, response, err);
    result.debug_json = json({
        {"endpoint", endpoint},
        {"http_status", status},
        {"response", response},
        {"payload_preview", payload}
    }).dump();
    if (!ok || status < 200 || status >= 400) {
      std::ostringstream ss;
      ss << "Google Forms response endpoint submit failed";
      if (!err.empty()) ss << ": " << err;
      if (status) ss << " (HTTP " << status << ")";
      result.error = ss.str();
      return result;
    }
    result.ok = true;
    result.submitted = true;
    return result;
  }

  result.error = "Google Forms API response submit is not enabled for this form. Use dry_run or a mapping with submit_mode=response_endpoint.";
  return result;
}

