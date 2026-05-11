#include "NotificationService.h"

#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;

static std::string level_emoji(const std::string& level) {
  if (level == "critical") return "\xF0\x9F\x94\xB4";
  if (level == "high")     return "\xF0\x9F\x9F\xA0";
  if (level == "medium")   return "\xF0\x9F\x9F\xA1";
  return "\xE2\x9A\xAB";
}

static int count_attachments_json(const std::string& json_str) {
  if (json_str.empty() || json_str == "[]") return 0;
  int count = 0;
  for (char c : json_str) if (c == '{') count++;
  return count;
}

std::string notification_service::format_message(const stored_email& email,
                                                   const email_analysis& analysis) const {
  std::ostringstream msg;
  msg << level_emoji(email.importance_level)
      << " *" << (email.subject.empty() ? "(без темы)" : email.subject) << "*\n";
  msg << "\xF0\x9F\x93\xA7 " << email.from_addr << "\n";
  if (!email.date_iso.empty()) msg << "\xF0\x9F\x95\x90 " << email.date_iso << "\n";

  if (!email.category.empty() && email.category != "other")
    msg << "\xF0\x9F\x8F\xB7 " << email.category << "\n";

  if (!analysis.reasons.empty()) {
    msg << "\n*Почему важно:*\n";
    for (const auto& r : analysis.reasons) msg << "\xE2\x80\xA2 " << r << "\n";
  }

  if (!analysis.safe_preview.empty()) {
    msg << "\n*Кратко:*\n" << analysis.safe_preview.substr(0, 250);
    if (analysis.safe_preview.size() > 250) msg << "…";
  } else if (!email.snippet.empty()) {
    msg << "\n" << email.snippet.substr(0, 180);
    if (email.snippet.size() > 180) msg << "…";
  }
  if (!analysis.deadline_text.empty())
    msg << "\n\xE2\x8F\xB0 *Срок:* " << analysis.deadline_text;

  int att_count = count_attachments_json(email.attachments_json);
  if (att_count > 0) msg << "\n\xF0\x9F\x93\x8E Вложений: " << att_count;
  return msg.str();
}

bool notification_service::notify_email(const stored_email& email,
                                         const email_analysis& analysis) {
  if (!bot.enabled()) return false;

  std::string text = format_message(email, analysis);


  std::string tok_open = store.save_telegram_callback_token(
      "mail:view",
      json{{"email_id", email.id}}.dump(),
      7200);
  std::string tok_read = store.save_telegram_callback_token(
      "mail:read",
      json{{"email_id", email.id}}.dump(),
      3600);
  std::string tok_archive = store.save_telegram_callback_token(
      "mail:archive",
      json{{"email_id", email.id}}.dump(),
      3600);

  int att_count = count_attachments_json(email.attachments_json);
  std::vector<std::vector<telegram_button>> keyboard;

  std::vector<telegram_button> row1;
  row1.push_back({"\xF0\x9F\x93\xA9 Открыть письмо", "mtok:" + tok_open, ""});
  keyboard.push_back(row1);

  keyboard.push_back({
    {"\xE2\x9C\x85 Прочитано", "mtok:" + tok_read,    ""},
    {"\xF0\x9F\x93\xA6 Архив",  "mtok:" + tok_archive, ""}
  });

  if (att_count > 0) {
    std::string tok_att = store.save_telegram_callback_token(
        "mail:attachments",
        json{{"email_id", email.id}}.dump(),
        7200);
    keyboard.push_back({{"\xF0\x9F\x93\x8E Вложения (" + std::to_string(att_count) + ")",
                          "mtok:" + tok_att, ""}});
  }

  std::string err;
  bool ok = bot.send_message(text, keyboard, err);

  notification_log log;
  log.uid     = email.uid;
  log.channel = "telegram";
  log.status  = ok ? "sent" : "failed";
  log.error   = err;
  store.log_notification(log);

  if (ok) {
    store.update_email_classification(
        email.id,
        email.classification_json,
        email.importance_level,
        email.importance_score,
        email.category,
        "important_notified");
  }
  return ok;
}
