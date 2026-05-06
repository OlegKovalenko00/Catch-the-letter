#pragma once

#include "../domain/Message.h"
#include "../app/Config.h"

#include <cstdint>
#include <string>
#include <vector>

struct imap_test_result {
  bool reachable = false;
  bool auth_ok = false;
  bool folder_ok = false;
  std::uint64_t max_uid = 0;
  std::string error;
};

class mail_client {
public:
  virtual ~mail_client() = default;
  virtual std::uint64_t fetch_max_uid() = 0;
  virtual std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) = 0;
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err);
imap_test_result test_imap_mailbox(const imap_config& cfg);
