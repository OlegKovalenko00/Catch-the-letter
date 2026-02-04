#include "Storage.h"

#include <sqlite3.h>

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>

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
  }

  ~sqlite_storage() override {
    if (db) sqlite3_close(db);
  }

  bool is_processed(const std::string& uid) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return false;
    const char* sql = "SELECT 1 FROM processed WHERE uid = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
  }

  void mark_processed(const message& msg) override {
    std::lock_guard<std::mutex> lock(mu);
    if (!db) return;
    const char* sql =
      "INSERT OR IGNORE INTO processed (uid, message_id, from_addr, subject, date_iso)"
      " VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
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
    const char* sql = "SELECT COUNT(*) FROM processed;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
  }

private:
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
