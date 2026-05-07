#include "WorkflowEngine.h"

#include "FormProviderRouter.h"
#include "FormUnderstandingEngine.h"

#include "../infra/GoogleFormsProvider.h"
#include "../infra/YandexFormsProvider.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

using nlohmann::json;

namespace {

std::string now_iso() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32]{};
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string render_template(std::string text, const message& msg) {
  replace_all(text, "{{from}}", msg.from);
  replace_all(text, "{{to}}", msg.to);
  replace_all(text, "{{subject}}", msg.subject);
  replace_all(text, "{{snippet}}", msg.snippet);
  replace_all(text, "{{date}}", msg.date_iso);
  replace_all(text, "{{uid}}", msg.uid);
  return text;
}

bool has_action(const match_result& result, const std::string& type) {
  for (const auto& action : result.actions) {
    if (action.type == type) return true;
  }
  return false;
}

bool has_confident_form_link(const message& msg) {
  for (const auto& item : msg.links) {
    if (item.confidence >= 0.75) return true;
  }
  return false;
}

std::string field_title(const form_field& field) {
  // Use field_label() from FormUnderstandingEngine — strips blank invisible chars
  // and falls back to api_question_id display for Yandex Forms.
  std::string lbl = field_label(field);
  if (!lbl.empty()) return lbl;
  if (!field.id.empty()) return field.id;
  return field.selector;
}

bool is_provider_session(const form_session& session) {
  return is_provider_submit_strategy(session.submit_strategy);
}

bool browser_field_missing_selector(const form_field& field) {
  if (!field.can_auto_fill) return false;
  if (field.value.empty() && field.values.empty()) return false;
  return field.selector.empty();
}

void apply_provider_metadata(form_session& session, const provider_inspect_result& inspect) {
  session.provider_type = inspect.provider;
  session.provider_name = inspect.provider == "yandex_forms" ? "Yandex Forms API" :
                          inspect.provider == "google_forms" ? "Google Forms API" :
                          inspect.provider;
  session.extraction_strategy = inspect.extraction_strategy;
  session.submit_strategy = inspect.submit_strategy;
  session.api_form_id = inspect.api_form_id;
  session.public_form_id = inspect.public_form_id;
  session.provider_debug_json = inspect.debug_json.empty() ? "{}" : inspect.debug_json;
  session.provider_error = inspect.error;
  session.captcha_required = inspect.captcha_required;
}

form_snapshot provider_snapshot_for_mapping(const provider_inspect_result& inspect,
                                            const std::string& fallback_url,
                                            const std::string& fallback_title) {
  form_snapshot snapshot = inspect.snapshot;
  if (snapshot.url.empty()) snapshot.url = fallback_url;
  if (snapshot.title.empty()) snapshot.title = fallback_title;
  snapshot.debug_json = inspect.debug_json.empty() ? "{}" : inspect.debug_json;
  snapshot.captcha_required = inspect.captcha_required;
  return snapshot;
}

}  // namespace

workflow_engine::workflow_engine(app_config cfg,
                                 storage& store,
                                 rule_engine& rules,
                                 email_classifier& classifier,
                                 browser_worker_client& browser,
                                 llm_client& llm,
                                 user_profile profile,
                                 telegram_bot* telegram)
  : cfg(std::move(cfg)),
    store(store),
    rules_engine(rules),
    classifier(classifier),
    browser(browser),
    llm(llm),
    profile(std::move(profile)),
    telegram(telegram) {}

void workflow_engine::append_event(std::string level,
                                   std::string type,
                                   std::string message_text,
                                   json data) {
  event_record event;
  event.level = std::move(level);
  event.type = std::move(type);
  event.message = std::move(message_text);
  event.data_json = data.dump();
  event.created_at = now_iso();
  store.append_event(event, cfg.events_limit);
}

bool workflow_engine::notify_text(const std::string& text, std::string& err) {
  if (telegram && telegram->enabled()) return telegram->send_message(text, err);
  std::cout << "[NOTIFY] " << text << std::endl;
  err.clear();
  return true;
}

void workflow_engine::notify_manual(const std::string& text) {
  std::string err;
  if (!notify_text(text, err)) {
    json data = json::object();
    data["error"] = err;
    append_event("error", "notification_failed", "Notification failed", data);
  }
}

void workflow_engine::notify_important(const message& msg, const email_analysis& analysis) {
  std::ostringstream ss;
  ss << "Важное письмо\n\n";
  if (!analysis.summary.empty()) ss << analysis.summary << "\n\n";
  ss << "Тема: " << msg.subject << "\nОт: " << msg.from;
  notify_manual(ss.str());
}

std::optional<message_link> workflow_engine::choose_form_link(const message& msg,
                                                              const email_analysis& analysis) const {
  // Minimum confidence to treat a URL as a fillable form.
  // URLs like forms.yandex.ru/admin/ (response viewers) score 0.0 and are excluded.
  constexpr double kMinConfidence = 0.5;

  if (!analysis.form_links.empty()) {
    auto best = std::max_element(
        analysis.form_links.begin(),
        analysis.form_links.end(),
        [](const message_link& a, const message_link& b) { return a.confidence < b.confidence; }
    );
    if (best->confidence >= kMinConfidence) return *best;
  }
  if (!msg.links.empty()) {
    auto best = std::max_element(
        msg.links.begin(),
        msg.links.end(),
        [](const message_link& a, const message_link& b) { return a.confidence < b.confidence; }
    );
    if (best->confidence >= kMinConfidence) return *best;
  }
  return std::nullopt;
}

workflow_result workflow_engine::handle_message(const message& msg, const std::vector<rule>& rules) {
  workflow_result out;
  auto match = rules_engine.apply(msg, rules);
  if (!match.matched) {
    store.mark_processed(msg, "ignored");
    json data = json::object();
    data["uid"] = msg.uid;
    data["mailbox_id"] = msg.mailbox_id;
    append_event("info", "message_ignored", msg.subject, data);
    out.status = "ignored";
    return out;
  }

  out.matched = true;
  {
    json data = json::object();
    data["uid"] = msg.uid;
    data["mailbox_id"] = msg.mailbox_id;
    append_event("info", "message_matched", msg.subject, data);
  }
  email_analysis analysis = classifier.analyze_email(msg);
  bool wants_classification = has_action(match, "classify");
  bool wants_form_detection = has_action(match, "detect_form");
  bool wants_notify = has_action(match, "notify");
  bool confident_form_link = has_confident_form_link(msg);
  bool should_start_form =
      analysis.kind == message_kind::form_request ||
      (wants_form_detection && confident_form_link);

  if (should_start_form) {
    if (start_form_workflow(msg, analysis, out.status)) {
      store.mark_processed(msg, out.status);
      return out;
    }
    store.mark_processed(msg, out.status.empty() ? "manual_required" : out.status);
    return out;
  }

  if (wants_form_detection && !confident_form_link) {
    json data = json::object();
    data["uid"] = msg.uid;
    data["mailbox_id"] = msg.mailbox_id;
    append_event("info", "form_detection_no_link", "No confident form link found", data);
  }

  if (analysis.kind == message_kind::important_notification || wants_notify || wants_classification) {
    bool sent_any = false;
    for (const auto& action : match.actions) {
      if (action.type != "notify") continue;
      std::string err;
      std::string text = render_template(action.text, msg);
      if (action.channel == "console") {
        std::cout << "[NOTIFY] " << text << std::endl;
        sent_any = true;
        continue;
      }
      if (action.channel == "telegram" && notify_text(text, err)) {
        sent_any = true;
      } else if (!err.empty()) {
        json data = json::object();
        data["error"] = err;
        append_event("error", "notification_failed", "Notification failed", data);
      }
    }
    if (!sent_any) notify_important(msg, analysis);
    store.mark_processed(msg, "important_notified");
    out.status = "important_notified";
    return out;
  }

  notify_important(msg, analysis);
  store.mark_processed(msg, "notified_unknown");
  out.status = "notified_unknown";
  return out;
}

bool workflow_engine::start_form_workflow(const message& msg,
                                          const email_analysis& analysis,
                                          std::string& status) {
  auto chosen = choose_form_link(msg, analysis);
  if (!chosen.has_value()) {
    notify_important(msg, analysis);
    json data = json::object();
    data["uid"] = msg.uid;
    append_event("info", "form_detection_no_link", "No form link found", data);
    status = "important_notified";
    return false;
  }

  if (cfg.security.mode == "paranoid") {
    form_session session;
    session.mailbox_id = msg.mailbox_id;
    session.message_uid = msg.uid;
    session.status = "manual_required";
    session.form_url = chosen->url;
    session.form_type = "manual";
    session.title = msg.subject;
    std::string id = store.create_form_session(session);
    notify_manual("Форма найдена, но paranoid mode требует ручного открытия:\n" + chosen->url);
    {
      json data = json::object();
      data["session_id"] = id;
      append_event("info", "paranoid_manual_required", "Paranoid mode manual required", data);
    }
    status = "manual_required";
    return false;
  }

  {
    json data = json::object();
    data["url"] = sanitize_url_for_log(chosen->url);
    data["uid"] = msg.uid;
    append_event("info", "form_detected", "Form link detected", data);
  }

  std::string reason;
  if (!is_allowed_url(chosen->url, cfg.security, reason)) {
    form_session session;
    session.mailbox_id = msg.mailbox_id;
    session.message_uid = msg.uid;
    session.status = "manual_required";
    session.form_url = chosen->url;
    session.form_type = "blocked";
    session.title = msg.subject;
    store.create_form_session(session);
    notify_manual("Ссылка формы заблокирована политикой безопасности: " + reason + "\n\n" + chosen->url);
    {
      json data = json::object();
      data["url"] = sanitize_url_for_log(chosen->url);
      append_event("warning", "url_blocked", reason, data);
    }
    status = "manual_required";
    return false;
  }

  form_provider_router router(cfg);
  provider_route route = router.route_for_url(chosen->url);
  if (route.provider_type == form_provider_type::yandex_forms || route.provider_type == form_provider_type::google_forms) {
    provider_inspect_result inspected;
    if (route.provider_type == form_provider_type::yandex_forms) {
      yandex_forms_provider provider(cfg.yandex_forms_api);
      inspected = provider.inspect(chosen->url);
    } else {
      google_forms_provider provider(cfg.google_forms_api);
      inspected = provider.inspect(chosen->url);
    }
    if (inspected.ok) {
      form_session session;
      session.mailbox_id = msg.mailbox_id;
      session.message_uid = msg.uid;
      session.form_url = inspected.snapshot.url.empty() ? chosen->url : inspected.snapshot.url;
      session.form_type = route.provider_type == form_provider_type::yandex_forms ? "yandex_forms" : "google_forms";
      session.title = inspected.snapshot.title.empty() ? msg.subject : inspected.snapshot.title;
      session.status = "waiting_user_review";
      apply_provider_metadata(session, inspected);
      form_snapshot mapped_snapshot = provider_snapshot_for_mapping(inspected, chosen->url, session.title);
      session.fields = llm.map_fields(msg, mapped_snapshot, profile);
      std::string id = store.create_form_session(session);
      append_event("info", "provider_form_inspected", "Known form provider inspected form",
                   {{"session_id", id}, {"provider", session.provider_type}, {"extraction_strategy", session.extraction_strategy}});
      auto saved = store.get_form_session(id);
      if (saved) send_form_review(*saved, reason);
      status = "form_waiting_user";
      return true;
    }
    if (!route.allow_browser_fallback || inspected.captcha_required) {
      form_session session;
      session.mailbox_id = msg.mailbox_id;
      session.message_uid = msg.uid;
      session.status = "manual_required";
      session.form_url = chosen->url;
      session.form_type = route.provider_type == form_provider_type::yandex_forms ? "yandex_forms" : "google_forms";
      session.title = msg.subject;
      apply_provider_metadata(session, inspected);
      session.submit_strategy = "manual";
      std::string id = store.create_form_session(session);
      notify_manual("Known Yandex/Google form provider is not configured. Browser fallback is disabled by default to avoid captcha/public UI.\n\n" + inspected.error + "\n\n" + chosen->url);
      append_event("warning", "provider_manual_required", inspected.error, {{"session_id", id}, {"provider", session.provider_type}});
      status = "manual_required";
      return false;
    }
  }

  std::string err;
  auto snapshot = browser.inspect_form(chosen->url, err);
  if (!snapshot.has_value()) {
    form_session session;
    session.mailbox_id = msg.mailbox_id;
    session.message_uid = msg.uid;
    session.status = "manual_required";
    session.form_url = chosen->url;
    {
      json data = json::object();
      data["url"] = sanitize_url_for_log(chosen->url);
      append_event("error", "browser_worker_unavailable", err, data);
    }
    status = "manual_required";
    return false;
  }

  {
    json data = json::object();
    data["url"] = sanitize_url_for_log(chosen->url);
    data["form_type"] = snapshot->form_type;
    append_event("info", "form_inspected", "Form inspected", data);
  }

  form_session session;
  session.mailbox_id = msg.mailbox_id;
  session.message_uid = msg.uid;
  session.form_url = snapshot->url.empty() ? chosen->url : snapshot->url;
  session.form_type = snapshot->form_type;
  session.title = snapshot->title.empty() ? msg.subject : snapshot->title;
  session.browser_session_id = snapshot->session_id;
  session.provider_type = "generic_browser";
  session.provider_name = "Generic Browser";
  session.extraction_strategy = snapshot->captcha_required ? "captcha_blocked" : "browser_dom";
  session.submit_strategy = "browser_worker";
  session.provider_debug_json = snapshot->debug_json;
  session.captcha_required = snapshot->captcha_required;

  if (snapshot->auth_required) {
    session.status = "waiting_auth";
    {
      json auth_state = json::object();
      auth_state["state"] = "required";
      auth_state["url"] = snapshot->final_url;
      session.auth_state_json = auth_state.dump();
    }
    std::string id = store.create_form_session(session);
    std::ostringstream ss;
    ss << "Форма требует авторизацию.\n\nПисьмо: " << msg.subject << "\nСайт: " << chosen->domain
       << "\n\nДля безопасного ввода логина и пароля откройте локальный Web UI: http://127.0.0.1:8080\n"
       << chosen->url;
    if (telegram && telegram->enabled()) {
      std::vector<std::vector<telegram_button>> buttons = {
          {{"Открыть самому", "form:" + id + ":manual"}},
          {{"Проверить после входа", "form:" + id + ":reinspect"}, {"Отмена", "form:" + id + ":cancel"}}
      };
      std::string send_err;
      if (!telegram->send_message(ss.str(), buttons, send_err)) {
        json data = json::object();
        data["error"] = send_err;
        append_event("error", "notification_failed", "Notification failed", data);
      }
    } else {
      notify_manual(ss.str());
    }
    {
      json data = json::object();
      data["session_id"] = id;
      data["url"] = sanitize_url_for_log(chosen->url);
      append_event("info", "waiting_auth", "Form requires auth", data);
    }
    status = "form_waiting_auth";
    return true;
  }

  if (snapshot->captcha_required) {
    session.status = "captcha_required";
    session.captcha_required = true;
    // keep browser session alive so user can interact and we can reinspect
    std::string id = store.create_form_session(session);
    auto saved = store.get_form_session(id);
    if (saved) {
      std::string send_err;
      send_captcha_message(*saved, send_err);
    }
    {
      json data = json::object();
      data["session_id"] = id;
      data["url"] = sanitize_url_for_log(chosen->url);
      append_event("warning", "captcha_required", "Captcha detected, waiting for user to pass it", data);
    }
    status = "captcha_required";
    return true;
  }

  if (snapshot->fields.empty()) {
    session.status = "manual_required";
    std::string id = store.create_form_session(session);
    notify_manual("Не удалось найти поля формы автоматически. Откройте вручную:\n" + chosen->url);
    {
      json data = json::object();
      data["session_id"] = id;
      data["url"] = sanitize_url_for_log(chosen->url);
      append_event("warning", "form_fields_not_found", "No interactive form fields found", data);
    }
    status = "manual_required";
    return false;
  }

  session.status = "waiting_user_review";
  form_snapshot mapped = *snapshot;
  mapped.fields = llm.map_fields(msg, *snapshot, profile);
  session.fields = mapped.fields;
  std::string id = store.create_form_session(session);
  auto saved = store.get_form_session(id);
  {
    json data = json::object();
    data["session_id"] = id;
    append_event("info", "form_waiting_user", "Form session created", data);
  }
  if (saved) {
    send_form_review(*saved, err);
  }
  status = "form_waiting_user";
  return true;
}

bool workflow_engine::send_form_review(const form_session& session, std::string& err) {
  std::ostringstream ss;
  int filled = 0;
  int needs_input = 0;
  int missing_required = 0;
  for (const auto& field : session.fields) {
    if (!field.value.empty() || !field.values.empty()) filled++;
    if (field.requires_user_input) needs_input++;
    if (field.required && field.value.empty() && field.values.empty()) missing_required++;
  }
  ss << "Найдена форма: " << session.title << "\n\n"
     << "Тип: " << (session.form_type.empty() ? "unknown" : session.form_type) << "\n"
     << "Автозаполнено: " << filled << "\n"
     << "Нужно ответить: " << needs_input << "\n"
     << "Обязательных пустых: " << missing_required << "\n\n"
     << "Ссылка: " << session.form_url << "\n\n";

  ss << "Предложенные значения:\n";
  int index = 1;
  bool any_known = false;
  for (const auto& field : session.fields) {
    if (field.value.empty()) {
      index++;
      continue;
    }
    any_known = true;
    ss << index << ". " << field_title(field) << ": " << field.value << "\n";
    index++;
  }
  if (!any_known) ss << "нет\n";

  ss << "\nНеизвестные поля:\n";
  index = 1;
  bool any_unknown = false;
  for (const auto& field : session.fields) {
    if (field.value.empty() || field.requires_user_input) {
      any_unknown = true;
      ss << index << ". " << field_title(field) << "\n";
    }
    index++;
  }
  if (!any_unknown) ss << "нет\n";

  ss << "\nЧто дальше:\n"
     << "1. Нажмите `Ответить здесь`, если нужно дополнить значения в Telegram.\n"
     << "2. Нажмите `Заполнить форму`, когда поля готовы.\n"
     << "3. Отправка будет отдельным подтверждением после заполнения.\n\n"
     << "Формат ответа:\n1: значение\nfield_id: значение";

  const std::string fill_label = is_provider_session(session) ? "Prepare response" : "Fill form";
  std::vector<std::vector<telegram_button>> buttons = {
      {{"Открыть Web UI", "form:" + session.id + ":open"}, {"Ответить здесь", "form:" + session.id + ":edit"}},
      {{"Remap", "form:" + session.id + ":remap"}, {"Заполнить форму", "form:" + session.id + ":fill"}},
      {{"Отложить", "form:" + session.id + ":defer"}},
      {{"Вручную", "form:" + session.id + ":manual"}, {"Отмена", "form:" + session.id + ":cancel"}}
  };

  if (buttons.size() > 1 && buttons[1].size() > 1) buttons[1][1].text = fill_label;
  if (telegram && telegram->enabled()) return telegram->send_message(ss.str(), buttons, err);
  std::cout << "[FORM REVIEW]\n" << ss.str() << std::endl;
  err.clear();
  return true;
}

bool workflow_engine::send_submit_confirmation(const form_session& session, std::string& err) {
  std::string text = "Форма заполнена, но ещё не отправлена.\n\n"
      "Проверьте результат в Web UI или подтвердите отправку здесь.\n\n" +
      session.title + "\n" + session.form_url;
  std::vector<std::vector<telegram_button>> buttons = {
      {{"Отправить", "form:" + session.id + ":submit"}, {"Проверить в Web UI", "form:" + session.id + ":open"}},
      {{"Открыть самому", "form:" + session.id + ":manual"}, {"Отмена", "form:" + session.id + ":cancel"}}
  };
  if (telegram && telegram->enabled()) return telegram->send_message(text, buttons, err);
  std::cout << "[SUBMIT CONFIRM]\n" << text << std::endl;
  err.clear();
  return true;
}

bool workflow_engine::create_demo_session(const std::string& url,
                                          const std::string& title,
                                          bool auth_demo,
                                          std::string& err) {
  auto snapshot = browser.inspect_form(url, err);
  if (!snapshot.has_value()) {
    json data = json::object();
    data["url"] = sanitize_url_for_log(url);
    append_event("error", "browser_worker_unavailable", err, data);
    return false;
  }

  message msg;
  msg.uid = std::string("demo-") + (auth_demo ? "auth-" : "form-") + std::to_string(std::time(nullptr));
  msg.mailbox_id = "demo";
  msg.provider = "demo";
  msg.subject = title;
  msg.body_text = "Demo form: " + url;
  msg.links.push_back({url, "", 1.0});

  form_session session;
  session.mailbox_id = msg.mailbox_id;
  session.message_uid = msg.uid;
  session.form_url = snapshot->url.empty() ? url : snapshot->url;
  session.form_type = snapshot->form_type;
  session.title = snapshot->title.empty() ? title : snapshot->title;
  session.browser_session_id = snapshot->session_id;
  session.provider_type = "generic_browser";
  session.provider_name = "Generic Browser";
  session.extraction_strategy = snapshot->captcha_required ? "captcha_blocked" : "browser_dom";
  session.submit_strategy = "browser_worker";
  session.provider_debug_json = snapshot->debug_json;
  session.captcha_required = snapshot->captcha_required;

  if (snapshot->auth_required) {
    session.status = "waiting_auth";
    {
      json auth_state = json::object();
      auth_state["state"] = "required";
      auth_state["url"] = snapshot->final_url;
      session.auth_state_json = auth_state.dump();
    }
    std::string id = store.create_form_session(session);
    {
      json data = json::object();
      data["session_id"] = id;
      append_event("info", "waiting_auth", "Demo auth form created", data);
    }
    err.clear();
    return true;
  }

  if (snapshot->captcha_required) {
    session.status = "captcha_required";
    session.captcha_required = true;
    std::string id = store.create_form_session(session);
    auto saved = store.get_form_session(id);
    if (saved) {
      std::string send_err;
      send_captcha_message(*saved, send_err);
    }
    {
      json data = json::object();
      data["session_id"] = id;
      append_event("warning", "captcha_required", "Demo captcha session created", data);
    }
    err.clear();
    return true;
  }

  session.status = "waiting_user_review";
  session.fields = llm.map_fields(msg, *snapshot, profile);
  std::string id = store.create_form_session(session);
  {
    json data = json::object();
    data["session_id"] = id;
    append_event("info", "form_waiting_user", "Demo form session created", data);
  }
  err.clear();
  return true;
}

bool workflow_engine::fill_form_after_review(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  if (is_provider_session(*session)) {
    auto validation = validate_understood_fields(session->fields);
    if (!validation.can_fill) {
      err = validation.missing_required.empty()
          ? "provider response validation failed"
          : "not all required fields are filled";
      return false;
    }
    for (const auto& field : session->fields) {
      if (field.diagnostic_only) continue;
      if (field.api_question_id.empty() && field.id.empty()) {
        err = "provider response requires api_question_id or mapping id";
        return false;
      }
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["provider"] = session->provider_type;
    payload["submit_strategy"] = session->submit_strategy;
    payload["answers"] = nlohmann::json::object();
    for (const auto& field : session->fields) {
      if (field.diagnostic_only) continue;
      std::string question_id = field.api_question_id.empty() ? field.id : field.api_question_id;
      if (question_id.empty()) continue;
      if (!field.values.empty()) payload["answers"][question_id] = field.values;
      else payload["answers"][question_id] = field.value;
    }
    session->provider_debug_json = nlohmann::json({{"payload_preview", payload}}).dump();
    session->status = "waiting_submit_confirm";
    store.update_form_session(*session);
    send_submit_confirmation(*session, err);
    append_event("info", "api_response_prepared", "Provider response prepared",
                 {{"session_id", session_id}, {"provider", session->provider_type}, {"submit_strategy", session->submit_strategy}});
    err.clear();
    return true;
  }
  if (session->browser_session_id.empty()) {
    err = "browser session id is empty";
    mark_manual_required(session_id, err);
    return false;
  }
  auto validation = validate_understood_fields(session->fields);
  if (!validation.can_fill) {
    if (!validation.missing_required.empty()) {
      err = "not all required fields are filled";
      notify_manual("Не все обязательные поля заполнены. Проверьте форму перед заполнением.");
    } else if (!validation.unsupported_required.empty()) {
      err = validation.unsupported_required.front().error;
      notify_manual("Обязательное поле нельзя заполнить автоматически. Откройте форму вручную.");
    } else if (!validation.invalid_options.empty()) {
      err = validation.invalid_options.front().error;
      notify_manual("В одном из полей есть ошибка значения. Проверьте форму перед заполнением.");
    } else {
      err = "form validation failed";
      notify_manual("Форма не прошла проверку перед заполнением.");
    }
    return false;
  }
  for (const auto& field : session->fields) {
    if (browser_field_missing_selector(field)) {
      err = "browser submit requires selectors for fields with values";
      mark_manual_required(session_id, err);
      return false;
    }
  }
  if (!browser.fill_form(session->browser_session_id, session->fields, err)) {
    session->status = "manual_required";
    store.update_form_session(*session);
    notify_manual("Не удалось заполнить форму автоматически. Откройте вручную:\n" + session->form_url);
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("error", "form_fill_failed", err, data);
    }
    return false;
  }
  session->status = "waiting_submit_confirm";
  store.update_form_session(*session);
  send_submit_confirmation(*session, err);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "form_filled", "Form filled", data);
  }
  return true;
}

bool workflow_engine::submit_form_after_confirm(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  if (cfg.security.require_confirmation_before_submit && session->status != "waiting_submit_confirm") {
    err = "submit confirmation is required";
    return false;
  }
  if (is_provider_session(*session)) {
    provider_submit_result submit;
    if (session->provider_type == "yandex_forms") {
      yandex_forms_provider provider(cfg.yandex_forms_api);
      submit = provider.submit(*session);
    } else if (session->provider_type == "google_forms") {
      google_forms_provider provider(cfg.google_forms_api);
      submit = provider.submit(*session);
    } else {
      err = "unknown provider submit strategy";
      return false;
    }
    session->provider_debug_json = submit.debug_json.empty() ? session->provider_debug_json : submit.debug_json;
    if (!submit.ok || !submit.submitted) {
      session->status = "failed";
      session->provider_error = submit.error;
      store.update_form_session(*session);
      err = submit.error.empty() ? "provider submit failed" : submit.error;
      append_event("error", "provider_submit_failed", err,
                   {{"session_id", session_id}, {"provider", session->provider_type}, {"submit_strategy", session->submit_strategy}});
      return false;
    }
    session->status = "submitted";
    store.update_form_session(*session);
    notify_manual("Form submitted via provider.\n\n" + session->title);
    append_event("info", "provider_form_submitted", "Provider form submitted",
                 {{"session_id", session_id}, {"provider", session->provider_type}, {"submit_strategy", session->submit_strategy}});
    err.clear();
    return true;
  }
  auto submit = browser.submit_form_result(session->browser_session_id, err);
  if (submit.needs_next) {
    form_snapshot next_snapshot;
    next_snapshot.fields = submit.fields;
    session->fields = llm.map_fields(message{}, next_snapshot, profile);
    if (session->fields.empty()) session->fields = submit.fields;
    session->status = "waiting_user_review";
    store.update_form_session(*session);
    send_form_review(*session, err);
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("info", "form_needs_next", "Multipage form has additional fields", data);
    }
    err.clear();
    return true;
  }
  if (!submit.ok || !submit.submitted) {
    store.update_form_session_status(session_id, "failed");
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("error", "form_submit_failed", err.empty() ? submit.error : err, data);
    }
    return false;
  }
  store.update_form_session_status(session_id, "submitted");
  std::string close_err;
  browser.close_session(session->browser_session_id, close_err);
  notify_manual("Форма отправлена.\n\n" + session->title);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "form_submitted", "Form submitted", data);
  }
  return true;
}

bool workflow_engine::mark_manual_required(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  store.update_form_session_status(session_id, "manual_required");
  std::string close_err;
  if (!session->browser_session_id.empty()) browser.close_session(session->browser_session_id, close_err);
  notify_manual("Откройте форму вручную:\n" + session->form_url);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "form_manual_required", "Form marked manual", data);
  }
  err.clear();
  return true;
}

bool workflow_engine::cancel_form(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  store.update_form_session_status(session_id, "cancelled");
  std::string close_err;
  if (!session->browser_session_id.empty()) browser.close_session(session->browser_session_id, close_err);
  notify_manual("Работа с формой отменена.\n\n" + session->title);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "form_cancelled", "Form cancelled", data);
  }
  err.clear();
  return true;
}

bool workflow_engine::reinspect_after_auth(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  auto snapshot = browser.reinspect_form(session->browser_session_id, err);
  if (!snapshot) {
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("error", "auth_reinspect_failed", err, data);
    }
    return false;
  }
  if (snapshot->auth_required) {
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("info", "auth_still_required", "Auth is still required", data);
    }
    err = "auth is still required";
    return false;
  }

  session->status = "waiting_user_review";
  session->form_type = snapshot->form_type;
  session->title = snapshot->title.empty() ? session->title : snapshot->title;
  session->fields = llm.map_fields(message{}, *snapshot, profile);
  store.update_form_session(*session);
  send_form_review(*session, err);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "auth_reinspect", "Form reinspected after auth", data);
  }
  err.clear();
  return true;
}

bool workflow_engine::submit_auth_credentials(const std::string& session_id,
                                              const std::string& username,
                                              const std::string& password,
                                              std::string& err) {
  if (!cfg.auth.enabled || !cfg.auth.allow_credentials_via_web) {
    err = "web credentials are disabled";
    return false;
  }
  if (cfg.auth.remember_credentials || cfg.auth.credentials_storage != "memory") {
    err = "encrypted credential storage is not implemented";
    return false;
  }
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  if (session->status != "waiting_auth") {
    err = "session is not waiting for auth";
    return false;
  }
  std::string status = browser.enter_credentials(session->browser_session_id, username, password, err);
  if (status == "authenticated") {
    return reinspect_after_auth(session_id, err);
  }
  if (status == "waiting_2fa") {
    store.update_form_session_status(session_id, "waiting_2fa");
    if (cfg.auth.two_factor_via_telegram && !cfg.telegram.chat_id.empty()) {
      telegram_dialog dialog;
      dialog.chat_id = cfg.telegram.chat_id;
      dialog.session_id = session_id;
      dialog.state = "waiting_2fa_code";
      dialog.payload_json = json({{"kind", "2fa"}}).dump();
      store.save_telegram_dialog(dialog);
    }
    notify_manual("Нужен одноразовый код 2FA. Введите его в Web UI или отправьте в Telegram, если это включено.");
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("info", "waiting_2fa", "Waiting for 2FA", data);
    }
    err.clear();
    return true;
  }
  store.update_form_session_status(session_id, "waiting_auth");
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("warning", "auth_failed", "Auth failed", data);
  }
  if (err.empty()) err = "auth failed";
  return false;
}

bool workflow_engine::submit_two_factor_code(const std::string& session_id,
                                             const std::string& code,
                                             std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  if (session->status != "waiting_2fa") {
    err = "session is not waiting for 2FA";
    return false;
  }
  std::string status = browser.enter_two_factor_code(session->browser_session_id, code, err);
  if (status == "authenticated") {
    return reinspect_after_auth(session_id, err);
  }
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("warning", "two_factor_failed", "2FA failed", data);
  }
  if (err.empty()) err = "2FA failed";
  return false;
}

bool workflow_engine::update_field_value(const std::string& session_id,
                                         const std::string& field_ref,
                                         const std::string& value,
                                         std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }

  bool updated = false;
  int index = 1;
  for (auto& field : session->fields) {
    if (field.id == field_ref || std::to_string(index) == field_ref) {
      field.value = value;
      field.requires_user_input = false;
      if (!value.empty()) field.can_auto_fill = true;
      field.user_modified = true;
      field.source = "user";
      field.reason = "edited by user";
      field.validation_error.clear();
      updated = true;
      break;
    }
    index++;
  }
  if (!updated) {
    err = "field not found";
    return false;
  }
  store.update_form_session(*session);
  {
    json data = json::object();
    data["session_id"] = session_id;
    data["field"] = field_ref;
    append_event("info", "form_field_updated", "Form field updated", data);
  }
  err.clear();
  return true;
}

bool workflow_engine::remap_form_fields(const std::string& session_id, bool force, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  message msg;
  msg.uid = session->message_uid;
  msg.mailbox_id = session->mailbox_id;
  msg.subject = session->title;
  msg.body_text = "Form: " + session->form_url;
  msg.links.push_back({session->form_url, "", 0.95});

  form_snapshot snapshot;
  snapshot.url = session->form_url;
  snapshot.form_type = session->form_type;
  snapshot.title = session->title;
  snapshot.fields = session->fields;

  std::map<std::string, form_field> previous_by_id;
  for (const auto& field : session->fields) previous_by_id[field.id] = field;
  auto remapped = llm.map_fields(msg, snapshot, profile);
  form_understanding_options options;
  options.force = force;
  options.preserve_user_edits = !force;
  finalize_form_understanding(remapped, profile, previous_by_id, options);
  auto validation = validate_understood_fields(remapped);
  session->fields = std::move(remapped);
  store.update_form_session(*session);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "form_remapped", "Form fields remapped from Telegram", data);
  }
  std::ostringstream ss;
  auto summary = mapping_summary_to_json(session->fields, validation);
  ss << "Сопоставление обновлено.\n"
     << "Готово: " << summary.value("ready", 0) << "\n"
     << "Нужно ответить: " << summary.value("needs_input", 0) << "\n"
     << "Низкая уверенность: " << summary.value("low_confidence", 0);
  notify_text(ss.str(), err);
  auto saved = store.get_form_session(session_id);
  if (saved) send_form_review(*saved, err);
  err.clear();
  return true;
}

bool workflow_engine::test_browser(std::string& err) {
  return browser.health(err);
}

bool workflow_engine::test_llm(std::string& err) {
  message msg;
  msg.subject = "Important form";
  msg.body_text = "Please fill form";
  auto analysis = llm.analyze_email(msg);
  err.clear();
  return analysis.kind != message_kind::ignored;
}

bool workflow_engine::test_telegram(std::string& err) {
  if (!telegram || !telegram->enabled()) {
    err = "telegram disabled";
    return false;
  }
  return telegram->send_message("Catch the Letter: test message", err);
}

void workflow_engine::set_profile(user_profile next_profile) {
  profile = std::move(next_profile);
}

std::string workflow_engine::web_base_url() const {
  std::string base = cfg.http.web_public_base_url.empty()
      ? "http://127.0.0.1:" + std::to_string(cfg.http.port)
      : cfg.http.web_public_base_url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

bool workflow_engine::send_captcha_message(const form_session& session, std::string& err) {
  std::string base = web_base_url();

  std::string captcha_url = base + "/forms/" + session.id + "/captcha";
  std::string web_ui_url = base;
  if (!cfg.http.auth_token.empty()) {
    captcha_url += "?token=" + cfg.http.auth_token;
    web_ui_url += "?token=" + cfg.http.auth_token;
  }

  std::string text =
      "⚠️ Яндекс показал "
      "проверку \"Вы не "
      "робот\".\n"
      "Я не буду считать "
      "капчу полем формы.\n"
      "Откройте проверку, "
      "подтвердите, что "
      "вы не робот, затем "
      "нажмите \"Я "
      "прошёл "
      "проверку\". "
      "После этого я "
      "продолжу заполне"
      "ние формы "
      "автоматически.";

  std::vector<std::vector<telegram_button>> buttons = {
      {{"Открыть проверку", "", captcha_url},
       {"Я прошёл проверку", "form:" + session.id + ":captcha_passed", ""}},
      {{"Открыть Web UI", "", web_ui_url},
       {"Открыть оригинал", "", session.form_url}},
      {{"Отмена", "form:" + session.id + ":cancel", ""}}
  };

  if (cfg.telegram.captcha_remote_control_experimental) {
    buttons.insert(buttons.begin() + 1,
        {{"[Exp] Скриншот", "form:" + session.id + ":captcha_screenshot", ""}});
  }

  if (telegram && telegram->enabled()) return telegram->send_message(text, buttons, err);
  std::cout << "[CAPTCHA] " << session.form_url << std::endl;
  err.clear();
  return true;
}

bool workflow_engine::reinspect_after_captcha(const std::string& session_id, std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  if (session->browser_session_id.empty()) {
    err = "browser session is gone; open the form manually";
    return false;
  }

  auto snapshot = browser.reinspect_form(session->browser_session_id, err);
  if (!snapshot) {
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("error", "captcha_reinspect_failed", err, data);
    }
    return false;
  }

  if (snapshot->captcha_required) {
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("info", "captcha_still_required", "Captcha is still active", data);
    }
    std::string notify_err;
    notify_text(
        "Проверка всё ещё "
        "активна. "
        "Пройдите её в "
        "открытой сессии "
        "и попробуйте снова.",
        notify_err);
    err = "captcha is still active";
    return false;
  }

  if (snapshot->fields.empty()) {
    session->status = "manual_required";
    session->captcha_required = false;
    store.update_form_session(*session);
    notify_manual(
        "Проверка пройдена, "
        "но поля формы не "
        "найдены. "
        "Откройте вручную:\n" +
        session->form_url);
    {
      json data = json::object();
      data["session_id"] = session_id;
      append_event("warning", "captcha_passed_no_fields", "Captcha passed but no fields found", data);
    }
    err = "captcha passed but no fields found";
    return false;
  }

  session->status = "waiting_user_review";
  session->captcha_required = false;
  session->extraction_strategy = "browser_dom";
  session->form_type = snapshot->form_type;
  session->title = snapshot->title.empty() ? session->title : snapshot->title;
  session->fields = llm.map_fields(message{}, *snapshot, profile);
  store.update_form_session(*session);
  send_form_review(*session, err);
  {
    json data = json::object();
    data["session_id"] = session_id;
    append_event("info", "captcha_reinspect", "Captcha passed, form reinspected", data);
  }
  err.clear();
  return true;
}
