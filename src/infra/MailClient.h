#pragma once

#include "../domain/Message.h"
#include "../app/Config.h"

#include <vector>

class mail_client {
public:
  virtual ~mail_client() = default;
  virtual std::vector<message> fetch_unseen() = 0;
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err);
