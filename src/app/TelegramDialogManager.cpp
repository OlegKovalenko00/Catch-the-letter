#include "TelegramDialogManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

telegram_dialog_manager::telegram_dialog_manager(telegram_config cfg,
                                                 telegram_bot& bot,
                                                 workflow_engine& workflow,
                                                 storage& store)
  : cfg(std::move(cfg)), bot(bot), workflow(workflow), store(store) {}

telegram_dialog_manager::~telegram_dialog_manager() {
  stop();
}

void telegram_dialog_manager::start() {
  if (running || !cfg.enabled || !cfg.poll_updates || !bot.enabled()) return;
  running = true;
  worker = std::thread([this]() { loop(); });
}

void telegram_dialog_manager::stop() {
  if (!running) return;
  running = false;
  if (worker.joinable()) worker.join();
}

void telegram_dialog_manager::loop() {
  long long offset = 0;
  auto saved = store.get_runtime_value("telegram_update_offset");
  if (saved) {
    try {
      offset = std::stoll(*saved);
    } catch (...) {
      offset = 0;
    }
  }

  while (running) {
    std::string err;
    auto updates = bot.get_updates(offset, err);
    for (const auto& update : updates) {
      handle_update(update);
      offset = std::max(offset, update.update_id + 1);
      store.set_runtime_value("telegram_update_offset", std::to_string(offset));
    }
    std::this_thread::sleep_for(std::chrono::seconds(std::max(1, cfg.poll_interval_seconds)));
  }
}

void telegram_dialog_manager::handle_update(const telegram_update& update) {
  if (!update.callback_data.empty()) {
    handle_callback(update);
    return;
  }
  if (!update.text.empty()) handle_text(update);
}

void telegram_dialog_manager::handle_callback(const telegram_update& update) {
  std::string callback_err;
  auto answer = [&](const std::string& text) {
    bot.answer_callback_query(update.callback_query_id, text, callback_err);
  };
  const std::string prefix = "form:";
  if (update.callback_data.rfind(prefix, 0) != 0) {
    answer("Неизвестная команда");
    return;
  }
  std::string rest = update.callback_data.substr(prefix.size());
  auto pos = rest.find(':');
  if (pos == std::string::npos) {
    answer("Неизвестная команда");
    return;
  }
  std::string session_id = rest.substr(0, pos);
  std::string action = rest.substr(pos + 1);

  std::string err;
  if (action.rfind("set:", 0) == 0) {
    answer("");
    if (!handle_option_answer(update.chat_id, session_id, action.substr(4), err) && !err.empty()) {
      workflow.notify_text("Не удалось сохранить ответ: " + err, err);
    }
    return;
  }
  if (action == "open") {
    answer("");
    workflow.notify_text("Откройте локальный Web UI: http://127.0.0.1:8080\n\nФорма: " + session_id, err);
    return;
  }
  if (action == "defer") {
    answer("");
    workflow.notify_text("Форма отложена. Она останется в Active Forms в Web UI.", err);
    return;
  }
  if (action == "remap") {
    answer("");
    workflow.remap_form_fields(session_id, false, err);
    if (!err.empty()) workflow.notify_text("Не удалось обновить сопоставление: " + err, err);
    return;
  }
  if (action == "fill") {
    answer("");
    workflow.fill_form_after_review(session_id, err);
    if (!err.empty()) workflow.notify_text("Не удалось заполнить форму: " + err, err);
    return;
  }
  if (action == "submit") {
    answer("");
    workflow.submit_form_after_confirm(session_id, err);
    if (!err.empty()) workflow.notify_text("Не удалось отправить форму: " + err, err);
    return;
  }
  if (action == "manual") {
    answer("");
    workflow.mark_manual_required(session_id, err);
    return;
  }
  if (action == "reinspect") {
    answer("");
    workflow.reinspect_after_auth(session_id, err);
    if (!err.empty()) workflow.notify_text("Авторизация всё ещё требуется или не удалась: " + err, err);
    return;
  }
  if (action == "captcha_passed") {
    answer("");
    if (!workflow.reinspect_after_captcha(session_id, err) && !err.empty()) {
      if (err != "captcha is still active") {
        workflow.notify_text("Ошибка при проверке формы: " + err, err);
      }
    }
    return;
  }
  if (action == "captcha_screenshot") {
    answer("Скриншот...");
    auto session = store.get_form_session(session_id);
    if (!session || session->browser_session_id.empty()) {
      workflow.notify_text("Сессия браузера недоступна.", err);
      return;
    }
    std::string base = workflow.web_base_url();
    std::string screenshot_url = base + "/api/forms/" + session_id + "/screenshot";
    std::vector<std::vector<telegram_button>> buttons = {
        {{"Я прошёл проверку", "form:" + session_id + ":captcha_passed", ""},
         {"Обновить скриншот", "form:" + session_id + ":captcha_screenshot", ""}},
        {{"Открыть проверку", "", screenshot_url.substr(0, screenshot_url.find("/api"))}},
        {{"Отмена", "form:" + session_id + ":cancel", ""}}
    };
    workflow.notify_text(
        "Откройте ссылку для просмотра состояния браузера:\n" + screenshot_url,
        err);
    return;
  }
  if (action == "edit") {
    answer("");
    if (!send_next_answer_prompt(update.chat_id, session_id, err) && !err.empty()) {
      workflow.notify_text("Не удалось начать ввод значений: " + err, err);
    }
    return;
  }
  if (action == "cancel") {
    answer("");
    workflow.cancel_form(session_id, err);
    return;
  }
  answer("Неизвестная команда");
}

namespace {

bool has_field_value(const form_field& field) {
  return !field.value.empty() || !field.values.empty();
}

bool needs_telegram_answer(const form_field& field) {
  if (field.required && !has_field_value(field)) return true;
  if (field.requires_user_input && !has_field_value(field)) return true;
  return false;
}

std::string display_label(const form_field& field) {
  if (!field.label.empty()) return field.label;
  if (!field.question_block_text.empty()) return field.question_block_text;
  return field.id;
}

std::string option_display(const field_option& option) {
  if (!option.label.empty()) return option.label;
  if (!option.value.empty()) return option.value;
  return option.id;
}

std::string option_value(const field_option& option) {
  if (!option.value.empty()) return option.value;
  if (!option.label.empty()) return option.label;
  return option.id;
}

}  // namespace

bool telegram_dialog_manager::send_next_answer_prompt(const std::string& chat_id,
                                                      const std::string& session_id,
                                                      std::string& err) {
  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }

  for (const auto& field : session->fields) {
    if (!needs_telegram_answer(field)) continue;

    telegram_dialog dialog;
    dialog.chat_id = chat_id;
    dialog.session_id = session_id;
    dialog.state = "answer_field";
    dialog.payload_json = nlohmann::json({{"field_id", field.id}}).dump();
    store.save_telegram_dialog(dialog);

    std::ostringstream ss;
    ss << "Нужно заполнить поле:\n\n"
       << display_label(field) << "\n";
    if (field.required) ss << "Обязательное поле.\n";
    if (!field.reason.empty()) ss << "Почему: " << field.reason << "\n";
    if (!field.options.empty()) {
      ss << "\nВарианты:\n";
      int i = 1;
      for (const auto& option : field.options) ss << i++ << ". " << option_display(option) << "\n";
      if (field.type == "checkbox_group") {
        ss << "\nМожно ответить несколькими значениями через запятую.";
      }
    }
    ss << "\n\nМожно ответить текстом здесь или открыть Web UI: http://127.0.0.1:8080";

    std::vector<std::vector<telegram_button>> buttons;
    if ((field.type == "radio_group" || field.type == "select") && !field.options.empty()) {
      std::vector<telegram_button> row;
      for (std::size_t i = 0; i < field.options.size(); ++i) {
        row.push_back({option_display(field.options[i]), "form:" + session_id + ":set:" + field.id + ":" + std::to_string(i)});
        if (row.size() == 3) {
          buttons.push_back(row);
          row.clear();
        }
      }
      if (!row.empty()) buttons.push_back(row);
    }
    buttons.push_back({{"Открыть Web UI", "form:" + session_id + ":open"}, {"Отмена", "form:" + session_id + ":cancel"}});
    return bot.send_message(ss.str(), buttons, err);
  }

  store.clear_telegram_dialog(chat_id);
  std::vector<std::vector<telegram_button>> buttons = {
      {{"Заполнить форму", "form:" + session_id + ":fill"}, {"Открыть Web UI", "form:" + session_id + ":open"}},
      {{"Отмена", "form:" + session_id + ":cancel"}}
  };
  return bot.send_message("Все обязательные ответы заполнены. Форму можно заполнить, отправка будет отдельным подтверждением.", buttons, err);
}

bool telegram_dialog_manager::handle_option_answer(const std::string& chat_id,
                                                   const std::string& session_id,
                                                   const std::string& payload,
                                                   std::string& err) {
  auto sep = payload.rfind(':');
  if (sep == std::string::npos) {
    err = "invalid option callback";
    return false;
  }
  std::string field_id = payload.substr(0, sep);
  std::string index_text = payload.substr(sep + 1);
  std::size_t index = 0;
  try {
    index = static_cast<std::size_t>(std::stoul(index_text));
  } catch (...) {
    err = "invalid option index";
    return false;
  }

  auto session = store.get_form_session(session_id);
  if (!session) {
    err = "form session not found";
    return false;
  }
  auto it = std::find_if(session->fields.begin(), session->fields.end(), [&](const form_field& field) {
    return field.id == field_id;
  });
  if (it == session->fields.end() || index >= it->options.size()) {
    err = "option not found";
    return false;
  }
  if (!workflow.update_field_value(session_id, field_id, option_value(it->options[index]), err)) return false;
  return send_next_answer_prompt(chat_id, session_id, err);
}

bool telegram_dialog_manager::parse_field_line(const std::string& line,
                                               std::string& field_ref,
                                               std::string& value) {
  auto pos = line.find(':');
  if (pos == std::string::npos) return false;
  field_ref = line.substr(0, pos);
  value = line.substr(pos + 1);
  auto trim = [](std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  };
  trim(field_ref);
  trim(value);
  return !field_ref.empty();
}

void telegram_dialog_manager::handle_text(const telegram_update& update) {
  auto trim = [](std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
  };
  const std::string text = trim(update.text);
  if (text == "/start" || text == "/help") {
    std::string err;
    workflow.notify_text(
        "Catch the Letter\n\n"
        "/status — краткий статус\n"
        "/forms — активные формы\n"
        "/cancel — отменить текущий Telegram-диалог\n\n"
        "Пароли вводятся только в локальном Web UI: http://127.0.0.1:8080\n"
        "2FA-код можно отправлять только когда форма явно ждёт код.",
        err);
    return;
  }
  if (text == "/status") {
    std::string err;
    auto forms = store.list_active_form_sessions(false);
    std::ostringstream ss;
    ss << "Статус: сервис работает.\nАктивных форм: " << forms.size()
       << "\nWeb UI: http://127.0.0.1:8080";
    workflow.notify_text(ss.str(), err);
    return;
  }
  if (text == "/forms") {
    std::string err;
    auto forms = store.list_active_form_sessions(false);
    if (forms.empty()) {
      workflow.notify_text("Активных форм нет.", err);
      return;
    }
    std::ostringstream ss;
    ss << "Активные формы:\n\n";
    int index = 1;
    for (const auto& form : forms) {
      ss << index++ << ". " << (form.title.empty() ? form.form_url : form.title)
         << "\nСтатус: " << form.status
         << "\nID: " << form.id << "\n\n";
    }
    ss << "Откройте Web UI для подробной проверки: http://127.0.0.1:8080";
    workflow.notify_text(ss.str(), err);
    return;
  }
  if (text == "/cancel") {
    std::string err;
    store.clear_telegram_dialog(update.chat_id);
    workflow.notify_text("Текущий Telegram-диалог очищен. Активные формы не отменены.", err);
    return;
  }

  auto dialog = store.get_telegram_dialog_by_chat(update.chat_id);
  auto looks_like_secret = [](const std::string& text) {
    if (text.size() < 6 || text.size() > 128) return false;
    if (text.find(':') != std::string::npos) return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  };
  if (!dialog) {
    if (looks_like_secret(update.text)) {
      std::string err;
      workflow.notify_text("Пароль нельзя отправлять через Telegram. Введите его в локальном Web UI: http://127.0.0.1:8080", err);
    }
    return;
  }

  std::string err;
  if (dialog->state == "waiting_2fa_code") {
    if (!workflow.submit_two_factor_code(dialog->session_id, text, err)) {
      workflow.notify_text("Не удалось применить 2FA-код: " + err, err);
      return;
    }
    store.clear_telegram_dialog(update.chat_id);
    return;
  }

  if (dialog->state == "answer_field") {
    std::string field_id;
    try {
      auto payload = nlohmann::json::parse(dialog->payload_json.empty() ? "{}" : dialog->payload_json);
      field_id = payload.value("field_id", "");
    } catch (...) {
      field_id.clear();
    }
    if (field_id.empty()) {
      workflow.notify_text("Не удалось определить поле. Откройте Web UI: http://127.0.0.1:8080", err);
      return;
    }
    if (text.find(':') != std::string::npos) {
      std::istringstream input(text);
      std::string line;
      int changed = 0;
      while (std::getline(input, line)) {
        std::string field_ref;
        std::string value;
        if (!parse_field_line(line, field_ref, value)) continue;
        if (workflow.update_field_value(dialog->session_id, field_ref, value, err)) changed++;
      }
      if (changed == 0) {
        workflow.notify_text("Не удалось распознать значения. Можно просто отправить ответ для текущего поля обычным сообщением.", err);
        return;
      }
    } else if (!workflow.update_field_value(dialog->session_id, field_id, text, err)) {
      workflow.notify_text("Не удалось сохранить ответ: " + err, err);
      return;
    }
    send_next_answer_prompt(update.chat_id, dialog->session_id, err);
    return;
  }

  if (dialog->state != "waiting_field_values") {
    if (looks_like_secret(update.text)) {
      std::string err;
      workflow.notify_text("Пароль нельзя отправлять через Telegram. Введите его в локальном Web UI: http://127.0.0.1:8080", err);
    }
    return;
  }

  std::istringstream input(text);
  std::string line;
  int changed = 0;
  while (std::getline(input, line)) {
    std::string field_ref;
    std::string value;
    if (!parse_field_line(line, field_ref, value)) continue;
    if (workflow.update_field_value(dialog->session_id, field_ref, value, err)) changed++;
  }

  if (changed == 0) {
    workflow.notify_text("Не удалось распознать значения. Используйте формат `1: значение`.", err);
    return;
  }
  store.clear_telegram_dialog(update.chat_id);
  auto session = store.get_form_session(dialog->session_id);
  if (session) workflow.send_form_review(*session, err);
}
