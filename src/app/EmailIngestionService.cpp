#include "EmailIngestionService.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

std::string email_ingestion_service::ingest(const message& msg) {
  stored_email email;
  email.mailbox_id  = msg.mailbox_id.empty() ? "default" : msg.mailbox_id;
  email.uid         = msg.uid;
  email.message_id  = msg.message_id;
  email.from_addr   = msg.from;
  email.to_addr     = msg.to;
  email.subject     = msg.subject;
  email.date_iso    = msg.date_iso;
  email.snippet     = msg.snippet;
  email.status      = "new";

  int max_chars = cfg.mail_processing.max_body_chars_to_store;
  std::string body = msg.body_text.empty() ? msg.body : msg.body_text;
  if (max_chars > 0 && static_cast<int>(body.size()) > max_chars)
    body.resize(static_cast<size_t>(max_chars));
  if (cfg.mail_processing.store_body_for_important) email.body_text = body;

  if (!msg.links.empty()) {
    json links_arr = json::array();
    for (const auto& lnk : msg.links)
      links_arr.push_back({{"url", lnk.url}, {"domain", lnk.domain}, {"confidence", lnk.confidence}});
    email.links_json = links_arr.dump();
  }

  if (!msg.attachments.empty()) {
    json att_arr = json::array();
    for (const auto& att : msg.attachments)
      att_arr.push_back({{"filename", att.filename}, {"mime_type", att.mime_type},
                         {"size_bytes", att.size_bytes}, {"part_id", att.part_id}});
    email.attachments_json = att_arr.dump();
  }

  return store.save_email_message(email);
}
