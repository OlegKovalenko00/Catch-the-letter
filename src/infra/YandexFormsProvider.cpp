#include "YandexFormsProvider.h"

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

std::string trim_slashes(std::string s) {
  while (!s.empty() && s.front() == '/') s.erase(s.begin());
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string trim_base_url(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
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

form_field field_from_mapping(const json& item, const std::string& provider, const std::string& strategy) {
  form_field field;
  field.id = json_string(item, "id", json_string(item, "api_question_id"));
  field.api_question_id = json_string(item, "api_question_id", field.id);
  field.yandex_question_id = field.api_question_id;
  field.label = json_string(item, "label", field.api_question_id);
  field.normalized_label = json_string(item, "normalized_label");
  field.type = json_string(item, "type", "text");
  field.required = item.value("required", false);
  field.api_answer_type = json_string(item, "api_answer_type", field.type);
  field.provider = provider;
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
        if (!option.id.empty()) {
          field.api_option_ids.push_back(option.id);
          field.yandex_option_ids.push_back(option.id);
        }
      }
    }
  }
  return field;
}

json build_payload(const form_session& session) {
  json answers = json::object();
  for (const auto& field : session.fields) {
    if (field.diagnostic_only) continue;
    std::string question_id = field.api_question_id.empty() ? field.id : field.api_question_id;
    if (question_id.empty()) continue;
    if (!field.values.empty()) {
      answers[question_id] = field.values;
    } else {
      answers[question_id] = field.value;
    }
  }
  return {
      {"provider", "yandex_forms"},
      {"strategy", "yandex_forms_api"},
      {"public_form_id", session.public_form_id},
      {"api_form_id", session.api_form_id.empty() ? session.public_form_id : session.api_form_id},
      {"answers", answers}
  };
}

bool curl_json(const std::string& url,
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

std::vector<form_field> parse_api_questions(const json& root) {
  std::vector<form_field> fields;
  json questions = json::array();
  if (root.contains("questions") && root["questions"].is_array()) questions = root["questions"];
  else if (root.contains("items") && root["items"].is_array()) questions = root["items"];
  else if (root.contains("fields") && root["fields"].is_array()) questions = root["fields"];

  for (const auto& item : questions) {
    if (!item.is_object()) continue;
    form_field field;
    field.api_question_id = json_string(item, "id", json_string(item, "question_id", json_string(item, "api_question_id")));
    field.id = field.api_question_id;
    field.label = json_string(item, "label", json_string(item, "title", json_string(item, "text", field.id)));
    field.type = json_string(item, "type", "text");
    field.required = item.value("required", false);
    field.api_answer_type = field.type;
    field.provider = "yandex_forms";
    field.submit_strategy = "yandex_forms_api";
    field.virtual_field = true;
    if (item.contains("options") && item["options"].is_array()) {
      for (const auto& opt : item["options"]) {
        auto option = option_from_json(opt);
        if (!option.label.empty() || !option.value.empty()) field.options.push_back(option);
      }
    }
    if (!field.id.empty()) fields.push_back(std::move(field));
  }
  return fields;
}

}  // namespace

yandex_forms_provider::yandex_forms_provider(yandex_forms_api_config cfg) : cfg(std::move(cfg)) {}

bool yandex_forms_provider::can_handle(const std::string& url) const {
  return lower_ascii(url).find("forms.yandex.ru") != std::string::npos;
}

std::string yandex_forms_provider::extract_public_form_id(const std::string& url) const {
  const std::string lower = lower_ascii(url);
  std::size_t marker = lower.find("/u/");
  if (marker == std::string::npos) return "";
  std::size_t start = marker + 3;
  std::size_t end = url.find_first_of("/?#", start);
  std::string id = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  return trim_slashes(id);
}

provider_inspect_result yandex_forms_provider::inspect(const std::string& url) const {
  provider_inspect_result result;
  result.provider = "yandex_forms";
  result.submit_strategy = "yandex_forms_api";
  result.snapshot.url = url;
  result.snapshot.form_type = "yandex_forms";
  result.public_form_id = extract_public_form_id(url);
  result.snapshot.final_url = url;

  if (result.public_form_id.empty()) {
    result.manual_required = true;
    result.error = "Yandex Forms URL must contain /u/<public_form_id>/ for provider mode.";
    result.debug_json = json({{"url", url}, {"provider", result.provider}}).dump();
    return result;
  }

  json mapping_root;
  if (read_json_file(cfg.form_map_file, mapping_root) && mapping_root.contains(result.public_form_id)) {
    const auto& entry = mapping_root[result.public_form_id];
    result.ok = true;
    result.extraction_strategy = "yandex_forms_mapping";
    result.api_form_id = json_string(entry, "api_form_id", result.public_form_id);
    result.snapshot.title = json_string(entry, "title", "Yandex Forms");
    result.snapshot.fields.clear();
    if (entry.contains("questions") && entry["questions"].is_array()) {
      for (const auto& question : entry["questions"]) {
        result.snapshot.fields.push_back(field_from_mapping(question, result.provider, result.submit_strategy));
      }
    }
    result.debug_json = json({
        {"mapping_file", cfg.form_map_file},
        {"public_form_id", result.public_form_id},
        {"api_form_id", result.api_form_id},
        {"field_count", result.snapshot.fields.size()}
    }).dump();
    return result;
  }

  if (cfg.enabled && !cfg.oauth_token.empty()) {
    std::string err;
    std::string body;
    long status = 0;
    std::vector<std::string> headers = {
        "Accept: application/json",
        "Authorization: OAuth " + cfg.oauth_token
    };
    if (!cfg.org_id.empty()) headers.push_back("X-Org-ID: " + cfg.org_id);
    if (!cfg.cloud_org_id.empty()) headers.push_back("X-Cloud-Org-ID: " + cfg.cloud_org_id);
    const std::string endpoint = trim_base_url(cfg.base_url) + "/forms/" + result.public_form_id;
    if (curl_json(endpoint, "GET", "", headers, cfg.timeout_seconds, status, body, err) &&
        status >= 200 && status < 300) {
      try {
        json root = json::parse(body.empty() ? "{}" : body);
        result.ok = true;
        result.extraction_strategy = "yandex_forms_api";
        result.api_form_id = json_string(root, "id", result.public_form_id);
        result.snapshot.title = json_string(root, "title", "Yandex Forms");
        result.snapshot.fields = parse_api_questions(root);
        result.debug_json = json({{"metadata_endpoint", endpoint}, {"http_status", status}}).dump();
        return result;
      } catch (const std::exception& e) {
        result.error = std::string("Yandex Forms API metadata parse failed: ") + e.what();
      }
    } else {
      result.error = err.empty() ? "Yandex Forms API metadata request failed" : err;
      result.debug_json = json({{"metadata_endpoint", endpoint}, {"http_status", status}, {"response", body}}).dump();
    }
  }

  result.manual_required = true;
  result.error = "Yandex Forms provider requires API credentials or mapping file. Browser fallback disabled by default because SmartCaptcha may block public UI.";
  result.debug_json = json({
      {"mapping_file", cfg.form_map_file},
      {"api_enabled", cfg.enabled},
      {"oauth_configured", !cfg.oauth_token.empty()}
  }).dump();
  return result;
}

provider_submit_result yandex_forms_provider::submit(const form_session& session) const {
  provider_submit_result result;
  result.provider = "yandex_forms";
  result.submit_strategy = "yandex_forms_api";
  json payload = build_payload(session);
  result.debug_json = json({{"payload_preview", payload}, {"dry_run", cfg.dry_run}}).dump();

  if (cfg.dry_run) {
    result.ok = true;
    result.submitted = true;
    return result;
  }
  if (!cfg.enabled) {
    result.error = "Yandex Forms API is disabled.";
    return result;
  }
  if (cfg.oauth_token.empty()) {
    result.error = "Yandex Forms API submit requires YANDEX_FORMS_OAUTH_TOKEN.";
    return result;
  }
  std::string form_id = session.api_form_id.empty() ? session.public_form_id : session.api_form_id;
  if (form_id.empty()) {
    result.error = "Yandex Forms API submit requires api_form_id or public_form_id.";
    return result;
  }
  for (const auto& field : session.fields) {
    if (field.diagnostic_only) continue;
    if (field.api_question_id.empty() && field.id.empty()) {
      result.error = "Yandex Forms API submit requires api_question_id for every mapped field.";
      return result;
    }
  }

  std::string body;
  long status = 0;
  std::string err;
  std::vector<std::string> headers = {
      "Content-Type: application/json",
      "Accept: application/json",
      "Authorization: OAuth " + cfg.oauth_token
  };
  if (!cfg.org_id.empty()) headers.push_back("X-Org-ID: " + cfg.org_id);
  if (!cfg.cloud_org_id.empty()) headers.push_back("X-Cloud-Org-ID: " + cfg.cloud_org_id);
  const std::string endpoint = trim_base_url(cfg.base_url) + "/forms/" + form_id + "/responses";
  const bool ok = curl_json(endpoint, "POST", payload.dump(), headers, cfg.timeout_seconds, status, body, err);
  result.debug_json = json({
      {"endpoint", endpoint},
      {"http_status", status},
      {"response", body},
      {"payload_preview", payload}
  }).dump();
  if (!ok || status < 200 || status >= 300) {
    std::ostringstream ss;
    ss << "Yandex Forms API submit failed";
    if (!err.empty()) ss << ": " << err;
    if (status) ss << " (HTTP " << status << ")";
    result.error = ss.str();
    return result;
  }
  result.ok = true;
  result.submitted = true;
  return result;
}

