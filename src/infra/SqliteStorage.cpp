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
    json options = json::array();
    for (const auto& option : field.options) {
      options.push_back({{"label", option.label}, {"value", option.value}, {"selector", option.selector}, {"id", option.id}});
    }
    arr.push_back({
        {"id", field.id},
        {"selector", field.selector},
        {"label", field.label},
        {"normalized_label", field.normalized_label},
        {"type", field.type},
        {"required", field.required},
        {"options", options},
        {"value", field.value},
        {"values", field.values},
        {"semantic_key", field.semantic_key},
        {"mapped_profile_key", field.mapped_profile_key},
        {"suggested_value", field.suggested_value},
        {"option_value", field.option_value},
        {"confidence", field.confidence},
        {"source", field.source},
        {"reason", field.reason},
        {"risk", field.risk},
        {"requires_user_input", field.requires_user_input},
        {"can_auto_fill", field.can_auto_fill},
        {"unsupported_reason", field.unsupported_reason},
        {"user_modified", field.user_modified},
        {"validation_error", field.validation_error},
        {"question_block_text", field.question_block_text},
        {"placeholder", field.placeholder},
        {"aria_label", field.aria_label},
        {"nearby_text", field.nearby_text},
        {"yandex_question_id", field.yandex_question_id},
        {"yandex_option_ids", field.yandex_option_ids},
        {"api_question_id", field.api_question_id},
        {"api_answer_type", field.api_answer_type},
        {"api_option_ids", field.api_option_ids},
        {"provider", field.provider},
        {"submit_strategy", field.submit_strategy},
        {"semantic_key_hint", field.semantic_key_hint},
        {"virtual_field", field.virtual_field},
        {"diagnostic_only", field.diagnostic_only}
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
      field.normalized_label = item.value("normalized_label", "");
      field.type = item.value("type", "unknown");
      field.required = item.value("required", false);
      if (item.contains("options") && item["options"].is_array()) {
        for (const auto& opt : item["options"]) {
          field_option option;
          if (opt.is_string()) {
            option.label = opt.get<std::string>();
            option.value = option.label;
          } else if (opt.is_object()) {
            option.label = opt.value("label", "");
            option.value = opt.value("value", option.label);
            option.selector = opt.value("selector", "");
            option.id = opt.value("id", "");
          }
          if (!option.label.empty() || !option.value.empty()) field.options.push_back(std::move(option));
        }
      }
      field.value = item.value("value", "");
      if (item.contains("values") && item["values"].is_array()) {
        for (const auto& value : item["values"]) {
          if (value.is_string()) field.values.push_back(value.get<std::string>());
        }
      }
      field.semantic_key = item.value("semantic_key", "");
      field.mapped_profile_key = item.value("mapped_profile_key", "");
      field.suggested_value = item.value("suggested_value", "");
      field.option_value = item.value("option_value", "");
      field.confidence = item.value("confidence", 0.0);
      field.source = item.value("source", "");
      field.reason = item.value("reason", "");
      field.risk = item.value("risk", "");
      field.requires_user_input = item.value("requires_user_input", false);
      field.can_auto_fill = item.value("can_auto_fill", true);
      field.unsupported_reason = item.value("unsupported_reason", "");
      field.user_modified = item.value("user_modified", false);
      field.validation_error = item.value("validation_error", "");
      field.question_block_text = item.value("question_block_text", "");
      field.placeholder = item.value("placeholder", "");
      field.aria_label = item.value("aria_label", "");
      field.nearby_text = item.value("nearby_text", "");
      field.yandex_question_id = item.value("yandex_question_id", "");
      if (item.contains("yandex_option_ids") && item["yandex_option_ids"].is_array()) {
        for (const auto& value : item["yandex_option_ids"]) {
          if (value.is_string()) field.yandex_option_ids.push_back(value.get<std::string>());
        }
      }
      field.api_question_id = item.value("api_question_id", "");
      field.api_answer_type = item.value("api_answer_type", "");
      if (item.contains("api_option_ids") && item["api_option_ids"].is_array()) {
        for (const auto& value : item["api_option_ids"]) {
          if (value.is_string()) field.api_option_ids.push_back(value.get<std::string>());
        }
      }
      field.provider = item.value("provider", "");
      field.submit_strategy = item.value("submit_strategy", "");
      field.semantic_key_hint = item.value("semantic_key_hint", "");
      field.virtual_field = item.value("virtual_field", false);
      field.diagnostic_only = item.value("diagnostic_only", false);
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
      " provider_type TEXT,"
      " provider_name TEXT,"
      " extraction_strategy TEXT,"
      " submit_strategy TEXT,"
      " api_form_id TEXT,"
      " public_form_id TEXT,"
      " provider_debug_json TEXT,"
      " provider_error TEXT,"
      " captcha_required INTEGER NOT NULL DEFAULT 0,"
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

    const char* ddl_email_message =
      "CREATE TABLE IF NOT EXISTS email_message ("
      " id TEXT PRIMARY KEY,"
      " mailbox_id TEXT NOT NULL,"
      " uid TEXT NOT NULL,"
      " message_id TEXT,"
      " from_addr TEXT,"
      " to_addr TEXT,"
      " subject TEXT,"
      " date_iso TEXT,"
      " snippet TEXT,"
      " body_text TEXT,"
      " links_json TEXT NOT NULL DEFAULT '[]',"
      " attachments_json TEXT NOT NULL DEFAULT '[]',"
      " classification_json TEXT NOT NULL DEFAULT '{}',"
      " importance_level TEXT NOT NULL DEFAULT 'low',"
      " importance_score REAL NOT NULL DEFAULT 0.0,"
      " category TEXT NOT NULL DEFAULT 'other',"
      " status TEXT NOT NULL DEFAULT 'new',"
      " read_at TEXT,"
      " archived_at TEXT,"
      " muted_until TEXT,"
      " created_at TEXT NOT NULL,"
      " updated_at TEXT NOT NULL,"
      " UNIQUE(mailbox_id, uid)"
      ");";

    const char* ddl_email_attachment =
      "CREATE TABLE IF NOT EXISTS email_attachment ("
      " id TEXT PRIMARY KEY,"
      " email_id TEXT NOT NULL,"
      " mailbox_id TEXT NOT NULL,"
      " uid TEXT NOT NULL,"
      " part_id TEXT,"
      " filename TEXT,"
      " mime_type TEXT,"
      " size_bytes INTEGER NOT NULL DEFAULT 0,"
      " content_id TEXT,"
      " disposition TEXT,"
      " safe_to_preview INTEGER NOT NULL DEFAULT 0,"
      " downloaded INTEGER NOT NULL DEFAULT 0,"
      " local_path TEXT,"
      " sha256 TEXT,"
      " created_at TEXT NOT NULL"
      ");";

    const char* ddl_telegram_callback_token =
      "CREATE TABLE IF NOT EXISTS telegram_callback_token ("
      " token TEXT PRIMARY KEY,"
      " action TEXT NOT NULL,"
      " payload_json TEXT NOT NULL,"
      " expires_at TEXT NOT NULL,"
      " created_at TEXT NOT NULL"
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
      ddl_runtime_kv,
      ddl_email_message,
      ddl_email_attachment,
      ddl_telegram_callback_token
    };
    for (const char* ddl : ddl_more) {
      if (sqlite3_exec(db, ddl, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        err = "sqlite ddl failed: " + std::string(err_msg ? err_msg : "");
        sqlite3_free(err_msg);
        return;
      }
    }
    ensure_active_form_session_columns();
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
      " proposed_values_json, unknown_fields_json, auth_state_json, browser_session_id, "
      " provider_type, provider_name, extraction_strategy, submit_strategy, api_form_id, public_form_id, "
      " provider_debug_json, provider_error, captcha_required, created_at, updated_at)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
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
      "auth_state_json, browser_session_id, provider_type, provider_name, extraction_strategy, submit_strategy, "
      "api_form_id, public_form_id, provider_debug_json, provider_error, captcha_required, created_at, updated_at "
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

  std::vector<form_session> list_active_form_sessions(bool all = false) override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<form_session> result;
    if (!db) return result;
    const char* sql = all ?
      "SELECT id, mailbox_id, message_uid, status, form_url, form_type, title, fields_json, "
      "auth_state_json, browser_session_id, provider_type, provider_name, extraction_strategy, submit_strategy, "
      "api_form_id, public_form_id, provider_debug_json, provider_error, captcha_required, created_at, updated_at "
      "FROM active_form_session "
      "ORDER BY updated_at DESC;" :
      "SELECT id, mailbox_id, message_uid, status, form_url, form_type, title, fields_json, "
      "auth_state_json, browser_session_id, provider_type, provider_name, extraction_strategy, submit_strategy, "
      "api_form_id, public_form_id, provider_debug_json, provider_error, captcha_required, created_at, updated_at "
      "FROM active_form_session "
      "WHERE status IN ('waiting_user_review', 'waiting_auth', 'waiting_2fa', 'waiting_submit_confirm', 'manual_required', 'failed') "
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
      "proposed_values_json=?, unknown_fields_json=?, auth_state_json=?, browser_session_id=?, "
      "provider_type=?, provider_name=?, extraction_strategy=?, submit_strategy=?, api_form_id=?, public_form_id=?, "
      "provider_debug_json=?, provider_error=?, captcha_required=?, updated_at=? "
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
    const char* clear_sql = "DELETE FROM telegram_dialog WHERE chat_id = ? AND id <> ?;";
    sqlite3_stmt* clear_stmt = nullptr;
    if (sqlite3_prepare_v2(db, clear_sql, -1, &clear_stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(clear_stmt, 1, dialog.chat_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(clear_stmt, 2, dialog.id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(clear_stmt);
    }
    sqlite3_finalize(clear_stmt);

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


  std::string save_email_message(const stored_email& input) override {
    stored_email email = input;
    if (email.id.empty()) email.id = random_id("email_");
    if (email.created_at.empty()) email.created_at = now_iso();
    email.updated_at = now_iso();

    std::lock_guard<std::mutex> lock(mu);
    if (!db) return email.id;

    const char* sql =
      "INSERT INTO email_message "
      "(id,mailbox_id,uid,message_id,from_addr,to_addr,subject,date_iso,snippet,body_text,"
      " links_json,attachments_json,classification_json,importance_level,importance_score,"
      " category,status,read_at,archived_at,muted_until,created_at,updated_at) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(mailbox_id,uid) DO UPDATE SET "
      " message_id=excluded.message_id,from_addr=excluded.from_addr,to_addr=excluded.to_addr,"
      " subject=excluded.subject,date_iso=excluded.date_iso,"
      " snippet=CASE WHEN excluded.snippet!='' THEN excluded.snippet ELSE email_message.snippet END,"
      " body_text=CASE WHEN excluded.body_text!='' THEN excluded.body_text ELSE email_message.body_text END,"
      " links_json=excluded.links_json,attachments_json=excluded.attachments_json,"
      " updated_at=excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return email.id;
    bind_stored_email(stmt, email);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    const char* sel = "SELECT id FROM email_message WHERE mailbox_id=? AND uid=? LIMIT 1;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, email.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, email.uid.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW) email.id = text_column(stmt, 0);
      sqlite3_finalize(stmt);
    }
    return email.id;
  }

  void update_email_classification(const std::string& email_id,
                                    const std::string& classification_json,
                                    const std::string& importance_level,
                                    double importance_score,
                                    const std::string& category,
                                    const std::string& status) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE email_message SET "
      "classification_json=?,importance_level=?,importance_score=?,category=?,status=?,updated_at=? "
      "WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string cls = classification_json.empty() ? "{}" : classification_json;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, cls.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, importance_level.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, importance_score);
    sqlite3_bind_text(stmt, 4, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, email_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  std::optional<stored_email> get_email_message(const std::string& email_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql =
      "SELECT id,mailbox_id,uid,message_id,from_addr,to_addr,subject,date_iso,snippet,body_text,"
      "links_json,attachments_json,classification_json,importance_level,importance_score,"
      "category,status,read_at,archived_at,muted_until,created_at,updated_at "
      "FROM email_message WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, email_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return std::nullopt; }
    auto e = read_stored_email(stmt);
    sqlite3_finalize(stmt);
    return e;
  }

  std::optional<stored_email> get_email_by_mailbox_uid(const std::string& mailbox_id,
                                                         const std::string& uid) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql =
      "SELECT id,mailbox_id,uid,message_id,from_addr,to_addr,subject,date_iso,snippet,body_text,"
      "links_json,attachments_json,classification_json,importance_level,importance_score,"
      "category,status,read_at,archived_at,muted_until,created_at,updated_at "
      "FROM email_message WHERE mailbox_id=? AND uid=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return std::nullopt; }
    auto e = read_stored_email(stmt);
    sqlite3_finalize(stmt);
    return e;
  }

  std::vector<stored_email> list_emails(const email_list_filter& filter,
                                         int limit, int offset) override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<stored_email> result;
    if (!db) return result;

    std::string sql =
      "SELECT id,mailbox_id,uid,message_id,from_addr,to_addr,subject,date_iso,snippet,body_text,"
      "links_json,attachments_json,classification_json,importance_level,importance_score,"
      "category,status,read_at,archived_at,muted_until,created_at,updated_at "
      "FROM email_message WHERE 1=1";
    std::vector<std::string> binds;

    if (filter.status == "important") {
      sql += " AND importance_level IN ('critical','high') AND archived_at IS NULL";
    } else if (filter.status == "unread") {
      sql += " AND read_at IS NULL AND archived_at IS NULL";
    } else if (filter.status == "archived") {
      sql += " AND archived_at IS NOT NULL";
    } else {
      if (!filter.archived) sql += " AND archived_at IS NULL";
    }

    if (!filter.importance_level.empty()) {
      sql += " AND importance_level=?";
      binds.push_back(filter.importance_level);
    }
    if (!filter.mailbox_id.empty()) {
      sql += " AND mailbox_id=?";
      binds.push_back(filter.mailbox_id);
    }
    std::string now = now_iso();
    if (!filter.muted) {
      sql += " AND (muted_until IS NULL OR muted_until<?)";
      binds.push_back(now);
    }
    sql += " ORDER BY date_iso DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    int idx = 1;
    for (const auto& v : binds) sqlite3_bind_text(stmt, idx++, v.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit > 0 ? limit : 50);
    sqlite3_bind_int(stmt, idx,   offset >= 0 ? offset : 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(read_stored_email(stmt));
    sqlite3_finalize(stmt);
    return result;
  }

  std::vector<stored_email> search_emails(const std::string& query,
                                           int limit, int offset) override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<stored_email> result;
    if (!db || query.empty()) return result;
    const char* sql =
      "SELECT id,mailbox_id,uid,message_id,from_addr,to_addr,subject,date_iso,snippet,body_text,"
      "links_json,attachments_json,classification_json,importance_level,importance_score,"
      "category,status,read_at,archived_at,muted_until,created_at,updated_at "
      "FROM email_message "
      "WHERE subject LIKE ? OR from_addr LIKE ? OR snippet LIKE ? OR body_text LIKE ? "
      "ORDER BY date_iso DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    std::string pat = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, limit > 0 ? limit : 20);
    sqlite3_bind_int(stmt, 6, offset >= 0 ? offset : 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(read_stored_email(stmt));
    sqlite3_finalize(stmt);
    return result;
  }

  void mark_email_read(const std::string& email_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE email_message SET read_at=?,updated_at=? WHERE id=? AND read_at IS NULL;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void archive_email(const std::string& email_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE email_message SET archived_at=?,status='archived',updated_at=? "
      "WHERE id=? AND archived_at IS NULL;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void mute_email(const std::string& email_id, const std::string& until_iso) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE email_message SET muted_until=?,updated_at=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    std::string ts = now_iso();
    sqlite3_bind_text(stmt, 1, until_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  int count_unread_important() override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return 0;
    const char* sql =
      "SELECT COUNT(*) FROM email_message "
      "WHERE read_at IS NULL AND archived_at IS NULL "
      "AND importance_level IN ('critical','high');";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
  }


  void save_email_attachments(const std::string& email_id,
                               const std::vector<stored_attachment>& attachments) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "INSERT OR IGNORE INTO email_attachment "
      "(id,email_id,mailbox_id,uid,part_id,filename,mime_type,size_bytes,"
      " content_id,disposition,safe_to_preview,downloaded,local_path,sha256,created_at) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    for (const auto& att : attachments) {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;
      std::string id = att.id.empty() ? random_id("att_") : att.id;
      std::string ts = now_iso();
      sqlite3_bind_text(stmt, 1,  id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2,  email_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3,  att.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4,  att.uid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5,  att.part_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6,  att.filename.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7,  att.mime_type.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(att.size_bytes));
      sqlite3_bind_text(stmt, 9,  att.content_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 10, att.disposition.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt,  11, att.safe_to_preview ? 1 : 0);
      sqlite3_bind_int(stmt,  12, att.downloaded ? 1 : 0);
      sqlite3_bind_text(stmt, 13, att.local_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 14, att.sha256.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 15, ts.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }

  std::vector<stored_attachment> get_email_attachments(const std::string& email_id) override {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<stored_attachment> result;
    if (!db) return result;
    const char* sql =
      "SELECT id,email_id,mailbox_id,uid,part_id,filename,mime_type,size_bytes,"
      "content_id,disposition,safe_to_preview,downloaded,local_path,sha256,created_at "
      "FROM email_attachment WHERE email_id=? ORDER BY rowid;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, email_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(read_stored_attachment(stmt));
    sqlite3_finalize(stmt);
    return result;
  }

  std::optional<stored_attachment> get_attachment(const std::string& attachment_id) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    const char* sql =
      "SELECT id,email_id,mailbox_id,uid,part_id,filename,mime_type,size_bytes,"
      "content_id,disposition,safe_to_preview,downloaded,local_path,sha256,created_at "
      "FROM email_attachment WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, attachment_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return std::nullopt; }
    auto a = read_stored_attachment(stmt);
    sqlite3_finalize(stmt);
    return a;
  }

  void update_attachment_download(const std::string& attachment_id,
                                   const std::string& local_path,
                                   const std::string& sha256) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "UPDATE email_attachment SET downloaded=1,local_path=?,sha256=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, local_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, attachment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }


  std::string save_telegram_callback_token(const std::string& action,
                                            const std::string& payload_json,
                                            int ttl_seconds) override {
    std::string token = random_token();
    std::string ts      = now_iso();
    std::string expires = future_iso(ttl_seconds > 0 ? ttl_seconds : 300);

    std::lock_guard<std::mutex> lock(mu);
    if (!db) return token;
    const char* sql =
      "INSERT INTO telegram_callback_token(token,action,payload_json,expires_at,created_at) "
      "VALUES(?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return token;
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, payload_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, expires.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return token;
  }

  std::optional<telegram_callback_token_record> resolve_telegram_callback_token(
      const std::string& token) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return std::nullopt;
    std::string now = now_iso();
    const char* sql =
      "SELECT token,action,payload_json,expires_at,created_at "
      "FROM telegram_callback_token WHERE token=? AND expires_at>? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return std::nullopt; }
    telegram_callback_token_record rec;
    rec.token        = text_column(stmt, 0);
    rec.action       = text_column(stmt, 1);
    rec.payload_json = text_column(stmt, 2);
    rec.expires_at   = text_column(stmt, 3);
    rec.created_at   = text_column(stmt, 4);
    sqlite3_finalize(stmt);

    const char* del = "DELETE FROM telegram_callback_token WHERE token=?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, del, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, rec.token.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    return rec;
  }

  void cleanup_expired_callback_tokens() override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    std::string now = now_iso();
    const char* sql = "DELETE FROM telegram_callback_token WHERE expires_at<?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

private:
  void ensure_active_form_session_columns() {
    const char* statements[] = {
        "ALTER TABLE active_form_session ADD COLUMN provider_type TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN provider_name TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN extraction_strategy TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN submit_strategy TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN api_form_id TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN public_form_id TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN provider_debug_json TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN provider_error TEXT;",
        "ALTER TABLE active_form_session ADD COLUMN captcha_required INTEGER NOT NULL DEFAULT 0;"
    };
    for (const char* sql : statements) {
      char* alter_err = nullptr;
      if (sqlite3_exec(db, sql, nullptr, nullptr, &alter_err) != SQLITE_OK) {
        sqlite3_free(alter_err);
      }
    }
  }

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
    sqlite3_bind_text(stmt, 13, session.provider_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, session.provider_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, session.extraction_strategy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, session.submit_strategy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, session.api_form_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 18, session.public_form_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 19, session.provider_debug_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 20, session.provider_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 21, session.captcha_required ? 1 : 0);
    sqlite3_bind_text(stmt, 22, session.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 23, session.updated_at.c_str(), -1, SQLITE_TRANSIENT);
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
    sqlite3_bind_text(stmt, 12, session.provider_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, session.provider_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, session.extraction_strategy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, session.submit_strategy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, session.api_form_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, session.public_form_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 18, session.provider_debug_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 19, session.provider_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 20, session.captcha_required ? 1 : 0);
    sqlite3_bind_text(stmt, 21, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 22, session.id.c_str(), -1, SQLITE_TRANSIENT);
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
    session.provider_type = text_column(stmt, 10);
    session.provider_name = text_column(stmt, 11);
    session.extraction_strategy = text_column(stmt, 12);
    session.submit_strategy = text_column(stmt, 13);
    session.api_form_id = text_column(stmt, 14);
    session.public_form_id = text_column(stmt, 15);
    session.provider_debug_json = text_column(stmt, 16);
    session.provider_error = text_column(stmt, 17);
    session.captcha_required = sqlite3_column_int(stmt, 18) != 0;
    session.created_at = text_column(stmt, 19);
    session.updated_at = text_column(stmt, 20);
    return session;
  }

  static stored_email read_stored_email(sqlite3_stmt* stmt) {
    stored_email e;
    e.id               = text_column(stmt, 0);
    e.mailbox_id       = text_column(stmt, 1);
    e.uid              = text_column(stmt, 2);
    e.message_id       = text_column(stmt, 3);
    e.from_addr        = text_column(stmt, 4);
    e.to_addr          = text_column(stmt, 5);
    e.subject          = text_column(stmt, 6);
    e.date_iso         = text_column(stmt, 7);
    e.snippet          = text_column(stmt, 8);
    e.body_text        = text_column(stmt, 9);
    e.links_json       = text_column(stmt, 10);
    e.attachments_json = text_column(stmt, 11);
    e.classification_json = text_column(stmt, 12);
    e.importance_level = text_column(stmt, 13);
    e.importance_score = sqlite3_column_double(stmt, 14);
    e.category         = text_column(stmt, 15);
    e.status           = text_column(stmt, 16);
    e.read_at          = text_column(stmt, 17);
    e.archived_at      = text_column(stmt, 18);
    e.muted_until      = text_column(stmt, 19);
    e.created_at       = text_column(stmt, 20);
    e.updated_at       = text_column(stmt, 21);
    return e;
  }

  static stored_attachment read_stored_attachment(sqlite3_stmt* stmt) {
    stored_attachment a;
    a.id             = text_column(stmt, 0);
    a.email_id       = text_column(stmt, 1);
    a.mailbox_id     = text_column(stmt, 2);
    a.uid            = text_column(stmt, 3);
    a.part_id        = text_column(stmt, 4);
    a.filename       = text_column(stmt, 5);
    a.mime_type      = text_column(stmt, 6);
    a.size_bytes     = static_cast<std::size_t>(sqlite3_column_int64(stmt, 7));
    a.content_id     = text_column(stmt, 8);
    a.disposition    = text_column(stmt, 9);
    a.safe_to_preview = sqlite3_column_int(stmt, 10) != 0;
    a.downloaded     = sqlite3_column_int(stmt, 11) != 0;
    a.local_path     = text_column(stmt, 12);
    a.sha256         = text_column(stmt, 13);
    a.created_at     = text_column(stmt, 14);
    return a;
  }

  static void bind_stored_email(sqlite3_stmt* stmt, const stored_email& e) {
    sqlite3_bind_text(stmt, 1,  e.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2,  e.mailbox_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3,  e.uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4,  e.message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5,  e.from_addr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6,  e.to_addr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7,  e.subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8,  e.date_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9,  e.snippet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, e.body_text.c_str(), -1, SQLITE_TRANSIENT);
    std::string links  = e.links_json.empty()          ? "[]" : e.links_json;
    std::string atts   = e.attachments_json.empty()    ? "[]" : e.attachments_json;
    std::string cls    = e.classification_json.empty() ? "{}" : e.classification_json;
    std::string level  = e.importance_level.empty()    ? "low"   : e.importance_level;
    std::string cat    = e.category.empty()            ? "other" : e.category;
    std::string stat   = e.status.empty()              ? "new"   : e.status;
    sqlite3_bind_text(stmt, 11, links.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, atts.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, cls.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, level.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 15, e.importance_score);
    sqlite3_bind_text(stmt, 16, cat.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, stat.c_str(),   -1, SQLITE_TRANSIENT);
    if (e.read_at.empty())     sqlite3_bind_null(stmt, 18);
    else sqlite3_bind_text(stmt, 18, e.read_at.c_str(), -1, SQLITE_TRANSIENT);
    if (e.archived_at.empty()) sqlite3_bind_null(stmt, 19);
    else sqlite3_bind_text(stmt, 19, e.archived_at.c_str(), -1, SQLITE_TRANSIENT);
    if (e.muted_until.empty()) sqlite3_bind_null(stmt, 20);
    else sqlite3_bind_text(stmt, 20, e.muted_until.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 21, e.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 22, e.updated_at.c_str(), -1, SQLITE_TRANSIENT);
  }

  static std::string future_iso(int seconds) {
    using namespace std::chrono;
    auto tp = system_clock::now() + std::chrono::seconds(seconds);
    auto t  = system_clock::to_time_t(tp);
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

  static std::string random_token() {
    auto ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream ss;
    ss << 't' << std::hex
       << (static_cast<unsigned long long>(ns) & 0x3FFFFFFFFFFFFull);
    return ss.str();
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
