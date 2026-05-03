#include "TelegramDialogManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>

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
  bot.answer_callback_query(update.callback_query_id, "", callback_err);
  const std::string prefix = "form:";
  if (update.callback_data.rfind(prefix, 0) != 0) return;
  std::string rest = update.callback_data.substr(prefix.size());
  auto pos = rest.rfind(':');
  if (pos == std::string::npos) return;
  std::string session_id = rest.substr(0, pos);
  std::string action = rest.substr(pos + 1);

  std::string err;
  if (action == "fill") {
    workflow.fill_form_after_review(session_id, err);
    if (!err.empty()) workflow.notify_text("Не удалось заполнить форму: " + err, err);
    return;
  }
  if (action == "submit") {
    workflow.submit_form_after_confirm(session_id, err);
    if (!err.empty()) workflow.notify_text("Не удалось отправить форму: " + err, err);
    return;
  }
  if (action == "manual") {
    workflow.mark_manual_required(session_id, err);
    return;
  }
  if (action == "reinspect") {
    workflow.reinspect_after_auth(session_id, err);
    if (!err.empty()) workflow.notify_text("Авторизация всё ещё требуется или не удалась: " + err, err);
    return;
  }
  if (action == "edit") {
    telegram_dialog dialog;
    dialog.chat_id = update.chat_id;
    dialog.session_id = session_id;
    dialog.state = "waiting_field_values";
    store.save_telegram_dialog(dialog);
    workflow.notify_text("Отправьте значения строками вида:\n1: значение\nfield_id: значение", err);
    return;
  }
  if (action == "cancel") {
    workflow.cancel_form(session_id, err);
    return;
  }
  bot.answer_callback_query(update.callback_query_id, "Неизвестная команда", callback_err);
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
  auto dialog = store.get_telegram_dialog_by_chat(update.chat_id);
  if (!dialog) return;

  if (dialog->state == "waiting_2fa_code") {
    std::string err;
    if (!workflow.submit_two_factor_code(dialog->session_id, update.text, err)) {
      workflow.notify_text("Не удалось применить 2FA-код: " + err, err);
      return;
    }
    store.clear_telegram_dialog(update.chat_id);
    return;
  }

  if (dialog->state != "waiting_field_values") return;

  std::istringstream input(update.text);
  std::string line;
  std::string err;
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
