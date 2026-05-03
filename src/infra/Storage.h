#pragma once

#include "../domain/Message.h"
#include "../domain/Form.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct notification_log {
  std::string uid;
  std::string channel;
  std::string status;
  std::string error;
  std::string ts_iso;
};

struct mailbox_checkpoint {
  std::string mailbox_id;
  std::string uid_validity;
  std::uint64_t last_seen_uid = 0;
  std::string started_at;
  std::string updated_at;
};

struct event_record {
  long long id = 0;
  std::string level;
  std::string type;
  std::string message;
  std::string data_json = "{}";
  std::string created_at;
};

struct telegram_dialog {
  std::string id;
  std::string chat_id;
  std::string session_id;
  std::string state;
  std::string payload_json = "{}";
  std::string created_at;
  std::string updated_at;
};

class storage {
public:
  virtual ~storage() = default;
  virtual bool is_processed(const std::string& mailbox_id, const std::string& uid) = 0;
  virtual void mark_processed(const message& msg, const std::string& status) = 0;
  virtual void log_notification(const notification_log& rec) = 0;
  virtual int processed_count() const = 0;

  virtual std::optional<mailbox_checkpoint> load_checkpoint(const std::string& mailbox_id) = 0;
  virtual void save_checkpoint(const mailbox_checkpoint& checkpoint) = 0;
  virtual void append_event(const event_record& event, int limit) = 0;
  virtual std::vector<event_record> last_events(int limit) const = 0;
  virtual std::string create_form_session(const form_session& session) = 0;
  virtual std::optional<form_session> get_form_session(const std::string& id) = 0;
  virtual std::vector<form_session> list_active_form_sessions() = 0;
  virtual void update_form_session(const form_session& session) = 0;
  virtual void update_form_session_status(const std::string& id, const std::string& status) = 0;
  virtual void save_telegram_dialog(const telegram_dialog& dialog) = 0;
  virtual std::optional<telegram_dialog> get_telegram_dialog_by_chat(const std::string& chat_id) = 0;
  virtual void clear_telegram_dialog(const std::string& chat_id) = 0;
  virtual std::optional<std::string> get_runtime_value(const std::string& key) = 0;
  virtual void set_runtime_value(const std::string& key, const std::string& value) = 0;

  bool is_processed(const std::string& uid) {
    return is_processed("default", uid);
  }

  void mark_processed(const message& msg) {
    mark_processed(msg, "processed");
  }
};

storage* make_sqlite_storage(const std::string& path, std::string* err);
