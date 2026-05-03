#pragma once

#include "../domain/Message.h"
#include "../app/Config.h"

#include <cstdint>
#include <vector>

class mail_client {
public:
  virtual ~mail_client() = default;
  virtual std::uint64_t fetch_max_uid() = 0;
  virtual std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) = 0;
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err);
