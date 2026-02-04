#pragma once

#include "../domain/Message.h"

#include <string>

struct notification_log {
  std::string uid;
  std::string channel;
  std::string status;
  std::string error;
  std::string ts_iso;
};

class storage {
public:
  virtual ~storage() = default;
  virtual bool is_processed(const std::string& uid) = 0;
  virtual void mark_processed(const message& msg) = 0;
  virtual void log_notification(const notification_log& rec) = 0;
  virtual int processed_count() const = 0;
};

storage* make_sqlite_storage(const std::string& path, std::string* err);
