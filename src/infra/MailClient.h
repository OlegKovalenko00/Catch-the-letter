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

  bool latest_fetch_ok = false;
  bool latest_subject_present = false;
  bool latest_from_present = false;
  bool latest_body_present = false;
  int latest_body_length = 0;
  bool parse_suspect = false;

  std::string fetch_mode;
  bool incomplete_literal = false;
  int response_size = 0;
  int links_count = 0;
};


struct mail_fetch_result {
  bool ok = true;
  std::string error;
  std::string mailbox_id;
  std::uint64_t last_seen_uid = 0;
  std::uint64_t max_seen_uid = 0;
  std::string uid_validity;
  std::vector<std::string> searched_uids;
  std::vector<std::string> fetched_uids;
  std::vector<message>     messages;
  std::vector<std::string> failed_uids;
  std::vector<std::string> parse_failed_uids;
};

class mail_client {
public:
  virtual ~mail_client() = default;
  virtual std::uint64_t fetch_max_uid() = 0;
  virtual std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) = 0;
  virtual std::vector<message> fetch_last_n(int n) = 0;
  virtual std::string fetch_uid_validity() { return ""; }
  virtual void mark_message_seen(const std::string& uid) {}


  virtual mail_fetch_result fetch_after_uid_result(std::uint64_t last_seen_uid) {
    mail_fetch_result r;
    r.last_seen_uid = last_seen_uid;
    auto msgs = fetch_after_uid(last_seen_uid);
    for (auto& m : msgs) {
      r.fetched_uids.push_back(m.uid);
      if (m.parse_suspect) {
        r.parse_failed_uids.push_back(m.uid);
      } else {
        std::uint64_t uid_n = 0;
        try { if (!m.uid.empty()) uid_n = std::stoull(m.uid); } catch (...) {}
        r.max_seen_uid = std::max(r.max_seen_uid, uid_n);
        r.messages.push_back(std::move(m));
      }
    }
    return r;
  }
};

mail_client* make_mail_client_imap(const imap_config& cfg, std::string* err);
imap_test_result test_imap_mailbox(const imap_config& cfg);
