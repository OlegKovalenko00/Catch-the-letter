#pragma once

#include "../domain/Message.h"
#include "../domain/EmailAnalysis.h"
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

struct email_list_filter {
  std::string status;
  std::string importance_level;
  std::string mailbox_id;
  bool archived = false;
  bool muted = false;
};

struct stored_email {
  std::string id;
  std::string mailbox_id;
  std::string uid;
  std::string message_id;
  std::string from_addr;
  std::string to_addr;
  std::string subject;
  std::string date_iso;
  std::string snippet;
  std::string body_text;
  std::string links_json;
  std::string attachments_json;
  std::string classification_json;
  std::string importance_level;
  double importance_score = 0.0;
  std::string category;
  std::string status;
  std::string read_at;
  std::string archived_at;
  std::string muted_until;
  std::string created_at;
  std::string updated_at;
};

struct stored_attachment {
  std::string id;
  std::string email_id;
  std::string mailbox_id;
  std::string uid;
  std::string part_id;
  std::string filename;
  std::string mime_type;
  std::size_t size_bytes = 0;
  std::string content_id;
  std::string disposition;
  bool safe_to_preview = false;
  bool downloaded = false;
  std::string local_path;
  std::string sha256;
  std::string created_at;
};

struct telegram_callback_token_record {
  std::string token;
  std::string action;
  std::string payload_json;
  std::string expires_at;
  std::string created_at;
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
  virtual std::vector<form_session> list_active_form_sessions(bool all = false) = 0;
  virtual void update_form_session(const form_session& session) = 0;
  virtual void update_form_session_status(const std::string& id, const std::string& status) = 0;
  virtual void save_telegram_dialog(const telegram_dialog& dialog) = 0;
  virtual std::optional<telegram_dialog> get_telegram_dialog_by_chat(const std::string& chat_id) = 0;
  virtual void clear_telegram_dialog(const std::string& chat_id) = 0;
  virtual std::optional<std::string> get_runtime_value(const std::string& key) = 0;
  virtual void set_runtime_value(const std::string& key, const std::string& value) = 0;


  virtual std::string save_email_message(const stored_email& email) = 0;
  virtual void update_email_classification(const std::string& email_id,
                                           const std::string& classification_json,
                                           const std::string& importance_level,
                                           double importance_score,
                                           const std::string& category,
                                           const std::string& status) = 0;
  virtual std::optional<stored_email> get_email_message(const std::string& email_id) = 0;
  virtual std::optional<stored_email> get_email_by_mailbox_uid(const std::string& mailbox_id,
                                                                const std::string& uid) = 0;
  virtual std::vector<stored_email> list_emails(const email_list_filter& filter,
                                                int limit,
                                                int offset) = 0;
  virtual std::vector<stored_email> search_emails(const std::string& query,
                                                  int limit,
                                                  int offset) = 0;
  virtual void mark_email_read(const std::string& email_id) = 0;
  virtual void archive_email(const std::string& email_id) = 0;
  virtual void mute_email(const std::string& email_id, const std::string& until_iso) = 0;
  virtual int count_unread_important() = 0;


  virtual void save_email_attachments(const std::string& email_id,
                                      const std::vector<stored_attachment>& attachments) = 0;
  virtual std::vector<stored_attachment> get_email_attachments(const std::string& email_id) = 0;
  virtual std::optional<stored_attachment> get_attachment(const std::string& attachment_id) = 0;
  virtual void update_attachment_download(const std::string& attachment_id,
                                          const std::string& local_path,
                                          const std::string& sha256) = 0;


  virtual std::string save_telegram_callback_token(const std::string& action,
                                                    const std::string& payload_json,
                                                    int ttl_seconds) = 0;
  virtual std::optional<telegram_callback_token_record> resolve_telegram_callback_token(
      const std::string& token) = 0;
  virtual void cleanup_expired_callback_tokens() = 0;

  bool is_processed(const std::string& uid) {
    return is_processed("default", uid);
  }

  void mark_processed(const message& msg) {
    mark_processed(msg, "processed");
  }
};

storage* make_sqlite_storage(const std::string& path, std::string* err);
