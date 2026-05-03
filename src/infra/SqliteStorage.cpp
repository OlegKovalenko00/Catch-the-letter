#include "Storage.h"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

static std::string now_iso() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
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

static json form_fields_to_json(const std::vector<form_field>& fields) {
  json arr = json::array();
  for (const auto& field : fields) {
    arr.push_back({
        {"id", field.id},
        {"selector", field.selector},
        {"label", field.label},
        {"type", field.type},
        {"required", field.required},
        {"options", field.options},
        {"value", field.value},
        {"mapped_profile_key", field.mapped_profile_key},
        {"confidence", field.confidence},
        {"requires_user_input", field.requires_user_input}
    });
  }
  return arr;
}

static std::vector<form_field> form_fields_from_json(const std::string& text) {
  std::vector<form_field> fields;
  try {
    json arr = json::parse(text.empty() ? "[]" : text);
    if (!arr.is_array()) return fields;
    for (const auto& item : arr) {
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
      fields.push_back(std::move(field));
    }
  } catch (...) {
  }
  return fields;
}

static std::string random_id(const std::string& prefix) {
  auto now = std::chrono::system_clock::now().time_since_epoch().count();
  std::ostringstream ss;
  ss << prefix << now;
  return ss.str();
}

class sqlite_storage final : public storage {
public:
  explicit sqlite_storage(const std::string& path, std::string& err) {
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
      err = "sqlite open failed: " + std::string(sqlite3_errmsg(db));
      sqlite3_close(db);
      db = nullptr;
      return;
    }

    const char* ddl_processed =
      "CREATE TABLE IF NOT EXISTS processed ("
      " uid TEXT PRIMARY KEY,"
      " message_id TEXT,"
      " from_addr TEXT,"
      " subject TEXT,"
      " date_iso TEXT"
      ");";

    const char* ddl_notifications =
      "CREATE TABLE IF NOT EXISTS notifications ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " uid TEXT,"
      " channel TEXT,"
      " status TEXT,"
      " error TEXT,"
      " ts_iso TEXT"
      ");";

    const char* ddl_mailbox_checkpoint =
      "CREATE TABLE IF NOT EXISTS mailbox_checkpoint ("
      " mailbox_id TEXT PRIMARY KEY,"
      " uid_validity TEXT,"
      " last_seen_uid INTEGER NOT NULL DEFAULT 0,"
      " started_at TEXT NOT NULL,"
      " updated_at TEXT NOT NULL"
      ");";

    const char* ddl_processed_message =
      "CREATE TABLE IF NOT EXISTS processed_message ("
      " mailbox_id TEXT NOT NULL,"
      " message_uid TEXT NOT NULL,"
      " message_id TEXT,"
      " status TEXT NOT NULL,"
      " processed_at TEXT NOT NULL,"
      " PRIMARY KEY (mailbox_id, message_uid)"
      ");";

    const char* ddl_active_form_session =
      "CREATE TABLE IF NOT EXISTS active_form_session ("
      " id TEXT PRIMARY KEY,"
      " mailbox_id TEXT NOT NULL,"
      " message_uid TEXT NOT NULL,"
      " status TEXT NOT NULL,"
      " form_url TEXT NOT NULL,"
      " form_type TEXT,"
      " title TEXT,"
      " fields_json TEXT NOT NULL,"
      " proposed_values_json TEXT NOT NULL,"
      " unknown_fields_json TEXT NOT NULL,"
      " auth_state_json TEXT NOT NULL,"
      " browser_session_id TEXT,"
      " created_at TEXT NOT NULL,"
      " updated_at TEXT NOT NULL"
      ");";

    const char* ddl_telegram_dialog =
      "CREATE TABLE IF NOT EXISTS telegram_dialog ("
      " id TEXT PRIMARY KEY,"
      " chat_id TEXT NOT NULL,"
      " session_id TEXT,"
      " state TEXT NOT NULL,"
      " payload_json TEXT NOT NULL,"
      " created_at TEXT NOT NULL,"
      " updated_at TEXT NOT NULL"
      ");";

    const char* ddl_event_log =
      "CREATE TABLE IF NOT EXISTS event_log ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " level TEXT NOT NULL,"
      " type TEXT NOT NULL,"
      " message TEXT NOT NULL,"
      " data_json TEXT NOT NULL,"
      " created_at TEXT NOT NULL"
      ");";

    const char* ddl_runtime_kv =
      "CREATE TABLE IF NOT EXISTS runtime_kv ("
      " key TEXT PRIMARY KEY,"
      " value TEXT NOT NULL,"
      " updated_at TEXT NOT NULL"
      ");";

    char* err_msg = nullptr;
    if (sqlite3_exec(db, ddl_processed, nullptr, nullptr, &err_msg) != SQLITE_OK) {
      err = "sqlite ddl failed: " + std::string(err_msg ? err_msg : "");
      sqlite3_free(err_msg);
      return;
    }
    if (sqlite3_exec(db, ddl_notifications, nullptr, nullptr, &err_msg) != SQLITE_OK) {
      err = "sqlite ddl failed: " + std::string(err_msg ? err_msg : "");
      sqlite3_free(err_msg);
      return;
    }
    const char* ddl_more[] = {
      ddl_mailbox_checkpoint,
      ddl_processed_message,
      ddl_active_form_session,
      ddl_telegram_dialog,
      ddl_event_log,
      ddl_runtime_kv
    };
    for (const char* ddl : ddl_more) {
      if (sqlite3_exec(db, ddl, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        err = "sqlite ddl failed: " + std::string(err_msg ? err_msg : "");
        sqlite3_free(err_msg);
        return;
      }
    }
  }

  ~sqlite_storage() override {
    if (db) sqlite3_close(db);
  }

  bool is_processed(const std::string& mailbox_id, const std::string& uid) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return false;
    const char* sql =
      "SELECT 1 FROM processed_message "
      "WHERE mailbox_id = ? AND message_uid = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return true;

    const char* legacy_sql = "SELECT 1 FROM processed WHERE uid = ? LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, legacy_sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
  }

  void mark_processed(const message& msg, const std::string& status) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* processed_message_sql =
      "INSERT INTO processed_message "
      "(mailbox_id, message_uid, message_id, status, processed_at)"
      " VALUES (?, ?, ?, ?, ?)"
      " ON CONFLICT(mailbox_id, message_uid) DO UPDATE SET"
      " message_id = excluded.message_id,"
      " status = excluded.status,"
      " processed_at = excluded.processed_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, processed_message_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      std::string mailbox_id = msg.mailbox_id.empty() ? "default" : msg.mailbox_id;
      std::string ts = now_iso();
      sqlite3_bind_text(stmt, 1, mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, msg.uid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, msg.message_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, status.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    const char* legacy_sql =
      "INSERT OR IGNORE INTO processed (uid, message_id, from_addr, subject, date_iso)"
      " VALUES (?, ?, ?, ?, ?);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, legacy_sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, msg.uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, msg.message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, msg.from.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, msg.subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, msg.date_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void log_notification(const notification_log& rec) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "INSERT INTO notifications (uid, channel, status, error, ts_iso)"
      " VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, rec.uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rec.channel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, rec.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec.error.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = rec.ts_iso.empty() ? now_iso() : rec.ts_iso;
    sqlite3_bind_text(stmt, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  int processed_count() const override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return 0;
    const char* sql =
      "SELECT "
      " (SELECT COUNT(*) FROM processed_message) +"
      " (SELECT COUNT(*) FROM processed "
      "  WHERE uid NOT IN (SELECT message_uid FROM processed_message));";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
  }

  std::optional<mailbox_checkpoint> load_checkpoint(const std::string& mailbox_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;

    const char* sql =
      "SELECT mailbox_id, uid_validity, last_seen_uid, started_at, updated_at "
      "FROM mailbox_checkpoint WHERE mailbox_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, mailbox_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return std::nullopt;
    }

    mailbox_checkpoint checkpoint;
    checkpoint.mailbox_id = text_column(stmt, 0);
    checkpoint.uid_validity = text_column(stmt, 1);
    checkpoint.last_seen_uid = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
    checkpoint.started_at = text_column(stmt, 3);
    checkpoint.updated_at = text_column(stmt, 4);
    sqlite3_finalize(stmt);
    return checkpoint;
  }

  void save_checkpoint(const mailbox_checkpoint& checkpoint) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;

    const char* sql =
      "INSERT INTO mailbox_checkpoint "
      "(mailbox_id, uid_validity, last_seen_uid, started_at, updated_at)"
      " VALUES (?, ?, ?, ?, ?)"
      " ON CONFLICT(mailbox_id) DO UPDATE SET"
      " uid_validity = excluded.uid_validity,"
      " last_seen_uid = excluded.last_seen_uid,"
      " updated_at = excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, checkpoint.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, checkpoint.uid_validity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(checkpoint.last_seen_uid));
    sqlite3_bind_text(stmt, 4, checkpoint.started_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, checkpoint.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void append_event(const event_record& event, int limit) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    if (limit <= 0) limit = 200;

    const char* insert_sql =
      "INSERT INTO event_log(level, type, message, data_json, created_at) "
      "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      std::string ts = event.created_at.empty() ? now_iso() : event.created_at;
      std::string data = event.data_json.empty() ? "{}" : event.data_json;
      sqlite3_bind_text(stmt, 1, event.level.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, event.type.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, event.message.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, data.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    const char* trim_sql =
      "DELETE FROM event_log "
      "WHERE id NOT IN (SELECT id FROM event_log ORDER BY id DESC LIMIT ?);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, trim_sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  std::vector<event_record> last_events(int limit) const override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<event_record> result;
    if (!db) return result;
    if (limit <= 0) limit = 200;

    const char* sql =
      "SELECT id, level, type, message, data_json, created_at "
      "FROM event_log ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      event_record event;
      event.id = sqlite3_column_int64(stmt, 0);
      event.level = text_column(stmt, 1);
      event.type = text_column(stmt, 2);
      event.message = text_column(stmt, 3);
      event.data_json = text_column(stmt, 4);
      event.created_at = text_column(stmt, 5);
      result.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return result;
  }

  std::string create_form_session(const form_session& input) override {
    form_session session = input;
    if (session.id.empty()) session.id = random_id("form_");
    if (session.created_at.empty()) session.created_at = now_iso();
    if (session.updated_at.empty()) session.updated_at = session.created_at;
    if (session.auth_state_json.empty()) session.auth_state_json = "{}";

    std::lock_guard<std::mutex> lock(mu);
    if (!db) return session.id;

    const char* sql =
      "INSERT INTO active_form_session "
      "(id, mailbox_id, message_uid, status, form_url, form_type, title, fields_json, "
      " proposed_values_json, unknown_fields_json, auth_state_json, browser_session_id, created_at, updated_at)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return session.id;
    bind_form_session(stmt, session);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return session.id;
  }

  std::optional<form_session> get_form_session(const std::string& id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql =
      "SELECT id, mailbox_id, message_uid, status, form_url, form_type, title, fields_json, "
      "auth_state_json, browser_session_id, created_at, updated_at "
      "FROM active_form_session WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return std::nullopt;
    }
    form_session session = read_form_session(stmt);
    sqlite3_finalize(stmt);
    return session;
  }

  std::vector<form_session> list_active_form_sessions() override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<form_session> result;
    if (!db) return result;
    const char* sql =
      "SELECT id, mailbox_id, message_uid, status, form_url, form_type, title, fields_json, "
      "auth_state_json, browser_session_id, created_at, updated_at "
      "FROM active_form_session "
      "WHERE status NOT IN ('submitted', 'manual_required', 'failed') "
      "ORDER BY updated_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      result.push_back(read_form_session(stmt));
    }
    sqlite3_finalize(stmt);
    return result;
  }

  void update_form_session(const form_session& session) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE active_form_session SET "
      "mailbox_id=?, message_uid=?, status=?, form_url=?, form_type=?, title=?, fields_json=?, "
      "proposed_values_json=?, unknown_fields_json=?, auth_state_json=?, browser_session_id=?, updated_at=? "
      "WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    bind_form_session_update(stmt, session);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void update_form_session_status(const std::string& id, const std::string& status) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql = "UPDATE active_form_session SET status = ?, updated_at = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void save_telegram_dialog(const telegram_dialog& input) override {
    telegram_dialog dialog = input;
    if (dialog.id.empty()) dialog.id = random_id("dialog_");
    if (dialog.created_at.empty()) dialog.created_at = now_iso();
    if (dialog.updated_at.empty()) dialog.updated_at = dialog.created_at;
    if (dialog.payload_json.empty()) dialog.payload_json = "{}";

    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "INSERT INTO telegram_dialog (id, chat_id, session_id, state, payload_json, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET chat_id=excluded.chat_id, session_id=excluded.session_id, "
      "state=excluded.state, payload_json=excluded.payload_json, updated_at=excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, dialog.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dialog.chat_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dialog.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, dialog.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, dialog.payload_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, dialog.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, dialog.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  std::optional<telegram_dialog> get_telegram_dialog_by_chat(const std::string& chat_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql =
      "SELECT id, chat_id, session_id, state, payload_json, created_at, updated_at "
      "FROM telegram_dialog WHERE chat_id = ? ORDER BY updated_at DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, chat_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return std::nullopt;
    }
    telegram_dialog dialog;
    dialog.id = text_column(stmt, 0);
    dialog.chat_id = text_column(stmt, 1);
    dialog.session_id = text_column(stmt, 2);
    dialog.state = text_column(stmt, 3);
    dialog.payload_json = text_column(stmt, 4);
    dialog.created_at = text_column(stmt, 5);
    dialog.updated_at = text_column(stmt, 6);
    sqlite3_finalize(stmt);
    return dialog;
  }

  void clear_telegram_dialog(const std::string& chat_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql = "DELETE FROM telegram_dialog WHERE chat_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, chat_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  std::optional<std::string> get_runtime_value(const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql = "SELECT value FROM runtime_kv WHERE key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return std::nullopt;
    }
    std::string value = text_column(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
  }

  void set_runtime_value(const std::string& key, const std::string& value) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "INSERT INTO runtime_kv(key, value, updated_at) VALUES (?, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

private:
  static std::string text_column(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
  }

  static json proposed_values_json(const form_session& session) {
    json out = json::object();
    for (const auto& field : session.fields) {
      if (!field.value.empty()) out[field.id] = field.value;
    }
    return out;
  }

  static json unknown_fields_json(const form_session& session) {
    json out = json::array();
    for (const auto& field : session.fields) {
      if (field.requires_user_input) out.push_back(field.id);
    }
    return out;
  }

  static void bind_form_session(sqlite3_stmt* stmt, const form_session& session) {
    sqlite3_bind_text(stmt, 1, session.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session.message_uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, session.form_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, session.form_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, session.title.c_str(), -1, SQLITE_TRANSIENT);
    std::string fields = form_fields_to_json(session.fields).dump();
    std::string proposed = proposed_values_json(session).dump();
    std::string unknown = unknown_fields_json(session).dump();
    sqlite3_bind_text(stmt, 8, fields.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, proposed.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, unknown.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, session.auth_state_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, session.browser_session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, session.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, session.updated_at.c_str(), -1, SQLITE_TRANSIENT);
  }

  static void bind_form_session_update(sqlite3_stmt* stmt, const form_session& session) {
    std::string updated = now_iso();
    sqlite3_bind_text(stmt, 1, session.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.message_uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session.form_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, session.form_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, session.title.c_str(), -1, SQLITE_TRANSIENT);
    std::string fields = form_fields_to_json(session.fields).dump();
    std::string proposed = proposed_values_json(session).dump();
    std::string unknown = unknown_fields_json(session).dump();
    sqlite3_bind_text(stmt, 7, fields.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, proposed.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, unknown.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, session.auth_state_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, session.browser_session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, session.id.c_str(), -1, SQLITE_TRANSIENT);
  }

  static form_session read_form_session(sqlite3_stmt* stmt) {
    form_session session;
    session.id = text_column(stmt, 0);
    session.mailbox_id = text_column(stmt, 1);
    session.message_uid = text_column(stmt, 2);
    session.status = text_column(stmt, 3);
    session.form_url = text_column(stmt, 4);
    session.form_type = text_column(stmt, 5);
    session.title = text_column(stmt, 6);
    session.fields = form_fields_from_json(text_column(stmt, 7));
    session.auth_state_json = text_column(stmt, 8);
    session.browser_session_id = text_column(stmt, 9);
    session.created_at = text_column(stmt, 10);
    session.updated_at = text_column(stmt, 11);
    return session;
  }

  sqlite3* db = nullptr;
  mutable std::mutex mu;
};

storage* make_sqlite_storage(const std::string& path, std::string* err) {
  std::string e;
  auto* ptr = new sqlite_storage(path, e);
  if (!e.empty()) {
    if (err) *err = e;
    delete ptr;
    return nullptr;
  }
  return ptr;
}
