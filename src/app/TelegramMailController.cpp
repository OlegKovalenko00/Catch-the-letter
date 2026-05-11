#include "TelegramMailController.h"

#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;

static const int PAGE_SIZE = 5;


bool telegram_mail_controller::handle_command(const std::string& chat_id,
                                               const std::string& text) {
  if (text == "/mail" || text == "/menu") { cmd_mail(chat_id); return true; }
  if (text == "/important")               { cmd_important(chat_id); return true; }
  if (text == "/unread")                  { cmd_unread(chat_id); return true; }
  if (text == "/digest")                  { cmd_digest(chat_id); return true; }
  if (text == "/diagnostics")             { cmd_diagnostics(chat_id); return true; }
  if (text.rfind("/search ", 0) == 0) {
    cmd_search(chat_id, text.substr(8));
    return true;
  }
  return false;
}

bool telegram_mail_controller::handle_callback(const std::string& chat_id,
                                                const std::string& callback_query_id,
                                                const std::string& callback_data) {
  if (callback_data.rfind("mtok:", 0) == 0) {
    handle_mtok(chat_id, callback_query_id, callback_data.substr(5));
    return true;
  }
  if (callback_data.rfind("mail:", 0) == 0) {

    std::string rest = callback_data.substr(5);
    auto pos = rest.find(':');
    if (pos == std::string::npos) return false;
    std::string action   = rest.substr(0, pos);
    std::string email_id = rest.substr(pos + 1);
    std::string err;
    if (action == "read") {
      store.mark_email_read(email_id);
      bot.answer_callback_query(callback_query_id, "\xE2\x9C\x85 Отмечено прочитанным", err);
      return true;
    }
    if (action == "archive") {
      store.archive_email(email_id);
      bot.answer_callback_query(callback_query_id, "\xF0\x9F\x93\xA6 Архивировано", err);
      return true;
    }
    if (action == "view") {
      auto email = store.get_email_message(email_id);
      if (email) send_email_detail(chat_id, *email, 0);
      else bot.answer_callback_query(callback_query_id, "Письмо не найдено", err);
      return true;
    }
    if (action == "attachments") {
      auto email = store.get_email_message(email_id);
      if (email) send_email_attachments(chat_id, *email);
      else bot.answer_callback_query(callback_query_id, "Письмо не найдено", err);
      return true;
    }
    return false;
  }
  return false;
}


void telegram_mail_controller::cmd_mail(const std::string& ) {
  int unread = store.count_unread_important();
  std::ostringstream text;
  text << "\xF0\x9F\x93\xAC *Catch the Letter*\n\n";
  if (unread > 0)
    text << "\xF0\x9F\x94\x94 Непрочитанных важных: " << unread << "\n\n";
  text << "Выберите раздел:";

  std::vector<std::vector<telegram_button>> kb = {
    {{"\xF0\x9F\x94\xB4 Важные письма", "mail:list:important", ""},
     {"\xF0\x9F\x93\xA7 Непрочитанные",  "mail:list:unread",   ""}},
    {{"\xF0\x9F\x93\x8B Дайджест",       "mail:list:digest",   ""},
     {"\xF0\x9F\x94\x8D Поиск",          "mail:search",        ""}},
    {{"\xF0\x9F\xA9\xBA Диагностика",    "mail:diagnostics",   ""}},
  };
  std::string err;
  bot.send_message(text.str(), kb, err);
}

void telegram_mail_controller::cmd_important(const std::string& chat_id) {
  email_list_filter filter;
  filter.status = "important";
  auto emails = store.list_emails(filter, PAGE_SIZE, 0);
  send_email_list(chat_id, emails, "\xF0\x9F\x94\xB4 Важные письма");
}

void telegram_mail_controller::cmd_unread(const std::string& chat_id) {
  email_list_filter filter;
  filter.status = "unread";
  auto emails = store.list_emails(filter, PAGE_SIZE, 0);
  send_email_list(chat_id, emails, "\xF0\x9F\x93\xA7 Непрочитанные");
}

void telegram_mail_controller::cmd_digest(const std::string& chat_id) {
  email_list_filter filter;
  filter.status = "all";
  auto emails = store.list_emails(filter, 10, 0);
  send_email_list(chat_id, emails, "\xF0\x9F\x93\x8B Дайджест");
}

void telegram_mail_controller::cmd_search(const std::string& chat_id,
                                           const std::string& query) {
  if (query.empty()) {
    std::string err;
    bot.send_message("Укажите запрос: /search <текст>", err);
    return;
  }
  auto emails = store.search_emails(query, PAGE_SIZE, 0);
  send_email_list(chat_id, emails, "\xF0\x9F\x94\x8D Результаты: " + query);
}

void telegram_mail_controller::cmd_diagnostics(const std::string& ) {
  int unread_important = store.count_unread_important();

  email_list_filter f_all;
  f_all.status = "all";
  auto all_recent = store.list_emails(f_all, 1, 0);

  std::ostringstream text;
  text << "\xF0\x9F\xA9\xBA *Диагностика*\n\n";
  text << "Непрочитанных важных: " << unread_important << "\n";
  text << "Последнее письмо: "
       << (all_recent.empty() ? "нет" : all_recent[0].date_iso) << "\n";
  text << "\n_Web UI: http://127.0.0.1:" << cfg.http.port << "_";

  std::string err;
  bot.send_message(text.str(), {}, err);
}


void telegram_mail_controller::send_email_list(const std::string& ,
                                                const std::vector<stored_email>& emails,
                                                const std::string& title) {
  std::string err;
  if (emails.empty()) {
    bot.send_message(title + "\n\n_Писем нет._", {}, err);
    return;
  }
  for (const auto& email : emails) {
    std::ostringstream text;
    text << (email.importance_level == "critical" ? "\xF0\x9F\x94\xB4" :
             email.importance_level == "high"     ? "\xF0\x9F\x9F\xA0" : "\xE2\xAC\x9C")
         << " *" << (email.subject.empty() ? "(без темы)" : email.subject) << "*\n"
         << "\xF0\x9F\x93\xA7 " << email.from_addr << "\n";
    if (!email.date_iso.empty()) text << "\xF0\x9F\x95\x90 " << email.date_iso << "\n";
    if (!email.category.empty() && email.category != "other")
      text << "\xF0\x9F\x8F\xB7 " << email.category << "\n";
    if (!email.snippet.empty()) {
      std::string snip = email.snippet.substr(0, 120);
      if (email.snippet.size() > 120) snip += "…";
      text << snip;
    }


    int att_count = 0;
    if (!email.attachments_json.empty() && email.attachments_json != "[]") {
      for (char c : email.attachments_json) if (c == '{') att_count++;
    }
    if (att_count > 0)
      text << "\n\xF0\x9F\x93\x8E " << att_count << " вложен.";

    std::string tok_open = store.save_telegram_callback_token(
        "mail:view", json{{"email_id", email.id}}.dump(), 7200);
    std::string tok_read = store.save_telegram_callback_token(
        "mail:read", json{{"email_id", email.id}}.dump(), 3600);
    std::string tok_archive = store.save_telegram_callback_token(
        "mail:archive", json{{"email_id", email.id}}.dump(), 3600);

    std::vector<std::vector<telegram_button>> kb = {
      {{"\xF0\x9F\x93\xA9 Открыть", "mtok:" + tok_open,    ""},
       {"\xE2\x9C\x85",             "mtok:" + tok_read,    ""},
       {"\xF0\x9F\x93\xA6",         "mtok:" + tok_archive, ""}}
    };
    if (att_count > 0) {
      std::string tok_att = store.save_telegram_callback_token(
          "mail:attachments", json{{"email_id", email.id}}.dump(), 7200);
      kb.push_back({{"\xF0\x9F\x93\x8E Вложения",
                     "mtok:" + tok_att, ""}});
    }
    bot.send_message(text.str(), kb, err);
  }
}

void telegram_mail_controller::send_email_detail(const std::string& ,
                                                   const stored_email& email,
                                                   int page) {
  std::string body = email.body_text.empty() ? email.snippet : email.body_text;
  int total_pages = body.empty() ? 1
      : (static_cast<int>(body.size()) + PAGE_BODY_CHARS - 1) / PAGE_BODY_CHARS;
  if (page < 0) page = 0;
  if (page >= total_pages) page = total_pages - 1;

  std::ostringstream text;
  text << "\xF0\x9F\x93\xA9 *" << (email.subject.empty() ? "(без темы)" : email.subject) << "*\n";
  text << "\xF0\x9F\x93\xA7 " << email.from_addr << "\n";
  if (!email.date_iso.empty()) text << "\xF0\x9F\x95\x90 " << email.date_iso << "\n";
  if (!email.importance_level.empty() && email.importance_level != "low")
    text << "Важность: " << email.importance_level << "\n";
  if (!email.category.empty() && email.category != "other")
    text << "Категория: " << email.category << "\n";


  try {
    auto cls = json::parse(email.classification_json.empty() ? "{}" : email.classification_json);
    auto reasons = cls.value("reasons", json::array());
    if (!reasons.empty()) {
      text << "\n*Почему важно:*\n";
      for (const auto& r : reasons) {
        if (r.is_string()) text << "\xE2\x80\xA2 " << r.get<std::string>() << "\n";
      }
    }
    auto preview = cls.value("safe_preview", "");
    if (!preview.empty()) {
      text << "\n*Кратко:* " << preview.substr(0, 200) << "\n";
    }
  } catch (...) {}


  if (!body.empty()) {
    size_t start = static_cast<size_t>(page) * PAGE_BODY_CHARS;
    std::string page_text = body.substr(start, PAGE_BODY_CHARS);
    text << "\n*Текст (" << (page + 1) << "/" << total_pages << "):*\n" << page_text;
    if (start + PAGE_BODY_CHARS < body.size()) text << "…";
  }


  std::vector<std::vector<telegram_button>> kb;
  if (total_pages > 1) {
    std::vector<telegram_button> nav_row;
    if (page > 0) {
      std::string tok_prev = store.save_telegram_callback_token(
          "mail:view_page",
          json{{"email_id", email.id}, {"page", page - 1}}.dump(), 7200);
      nav_row.push_back({"\xE2\xAC\x85 Назад", "mtok:" + tok_prev, ""});
    }
    if (page < total_pages - 1) {
      std::string tok_next = store.save_telegram_callback_token(
          "mail:view_page",
          json{{"email_id", email.id}, {"page", page + 1}}.dump(), 7200);
      nav_row.push_back({"\xE2\x9E\xA1 Далее", "mtok:" + tok_next, ""});
    }
    if (!nav_row.empty()) kb.push_back(nav_row);
  }

  int att_count = 0;
  if (!email.attachments_json.empty() && email.attachments_json != "[]") {
    for (char c : email.attachments_json) if (c == '{') att_count++;
  }

  std::string tok_read = store.save_telegram_callback_token(
      "mail:read", json{{"email_id", email.id}}.dump(), 3600);
  std::string tok_archive = store.save_telegram_callback_token(
      "mail:archive", json{{"email_id", email.id}}.dump(), 3600);

  std::vector<telegram_button> action_row = {
    {"\xE2\x9C\x85 Прочитано", "mtok:" + tok_read,    ""},
    {"\xF0\x9F\x93\xA6 Архив",  "mtok:" + tok_archive, ""}
  };
  kb.push_back(action_row);

  if (att_count > 0) {
    std::string tok_att = store.save_telegram_callback_token(
        "mail:attachments", json{{"email_id", email.id}}.dump(), 7200);
    kb.push_back({{"\xF0\x9F\x93\x8E Вложения (" + std::to_string(att_count) + ")",
                   "mtok:" + tok_att, ""}});
  }

  std::string err;
  bot.send_message(text.str(), kb, err);
}

void telegram_mail_controller::send_email_attachments(const std::string& ,
                                                        const stored_email& email) {
  auto attachments = store.get_email_attachments(email.id);
  std::string err;
  if (attachments.empty()) {
    bot.send_message("\xF0\x9F\x93\x8E Вложений нет.", {}, err);
    return;
  }

  std::ostringstream text;
  text << "\xF0\x9F\x93\x8E *Вложения к письму:* "
       << (email.subject.empty() ? "(без темы)" : email.subject) << "\n\n";

  std::vector<std::vector<telegram_button>> kb;
  int idx = 1;
  for (const auto& att : attachments) {
    text << idx++ << ". " << (att.filename.empty() ? "файл" : att.filename);
    if (!att.mime_type.empty()) text << " (" << att.mime_type << ")";
    if (att.size_bytes > 0) {
      text << " — " << (att.size_bytes / 1024) << " KB";
    }
    text << "\n";


    bool can_send = att.safe_to_preview
        && att.downloaded
        && cfg.attachments.telegram_send_enabled
        && att.size_bytes <= static_cast<size_t>(cfg.attachments.max_telegram_size_mb) * 1024 * 1024;
    if (can_send) {
      std::string tok_send = store.save_telegram_callback_token(
          "mail:attachment_send",
          json{{"attachment_id", att.id}}.dump(), 3600);
      kb.push_back({{"\xF0\x9F\x93\xA4 Отправить: " + att.filename,
                     "mtok:" + tok_send, ""}});
    }
  }
  bot.send_message(text.str(), kb, err);
}

void telegram_mail_controller::handle_mtok(const std::string& chat_id,
                                            const std::string& callback_query_id,
                                            const std::string& token) {
  auto rec = store.resolve_telegram_callback_token(token);
  if (!rec) {
    std::string err;
    bot.answer_callback_query(callback_query_id, "Кнопка устарела. Повторите команду.", err);
    return;
  }
  std::string err;
  try {
    json payload = json::parse(rec->payload_json);
    std::string email_id = payload.value("email_id", "");

    if (rec->action == "mail:read") {
      store.mark_email_read(email_id);
      bot.answer_callback_query(callback_query_id, "\xE2\x9C\x85 Прочитано", err);

    } else if (rec->action == "mail:archive") {
      store.archive_email(email_id);
      bot.answer_callback_query(callback_query_id, "\xF0\x9F\x93\xA6 Архивировано", err);

    } else if (rec->action == "mail:view") {
      auto email = store.get_email_message(email_id);
      if (email) send_email_detail(chat_id, *email, 0);
      bot.answer_callback_query(callback_query_id, "", err);

    } else if (rec->action == "mail:view_page") {
      int page = payload.value("page", 0);
      auto email = store.get_email_message(email_id);
      if (email) send_email_detail(chat_id, *email, page);
      bot.answer_callback_query(callback_query_id, "", err);

    } else if (rec->action == "mail:attachments") {
      auto email = store.get_email_message(email_id);
      if (email) send_email_attachments(chat_id, *email);
      bot.answer_callback_query(callback_query_id, "", err);

    } else if (rec->action == "mail:attachment_send") {
      std::string attachment_id = payload.value("attachment_id", "");
      std::string att_err;
      auto att = store.get_attachment(attachment_id);
      if (!att) {
        bot.answer_callback_query(callback_query_id, "Вложение не найдено", err);
      } else if (!att->downloaded || att->local_path.empty()) {
        bot.answer_callback_query(callback_query_id,
            "Вложение не скачано. Попробуйте через Web UI.", err);
      } else {
        bot.answer_callback_query(callback_query_id, "\xF0\x9F\x93\xA4 Отправляю...", err);
        bot.send_document(att->local_path, att->filename, att_err);
        if (!att_err.empty()) {
          bot.send_message("Не удалось отправить: " + att_err, err);
        }
      }

    } else if (rec->action == "mail:list:important") {
      bot.answer_callback_query(callback_query_id, "", err);
      cmd_important(chat_id);
    } else if (rec->action == "mail:list:unread") {
      bot.answer_callback_query(callback_query_id, "", err);
      cmd_unread(chat_id);
    } else if (rec->action == "mail:list:digest") {
      bot.answer_callback_query(callback_query_id, "", err);
      cmd_digest(chat_id);
    } else if (rec->action == "mail:diagnostics") {
      bot.answer_callback_query(callback_query_id, "", err);
      cmd_diagnostics(chat_id);
    } else {
      bot.answer_callback_query(callback_query_id, "Неизвестное действие", err);
    }
  } catch (...) {
    bot.answer_callback_query(callback_query_id, "Ошибка обработки", err);
  }
}
