#include "AttachmentService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

static std::string compute_sha256_stub(const std::string& ) {


  return "";
}

std::string attachment_service::ext_lower(const std::string& filename) const {
  auto dot = filename.rfind('.');
  if (dot == std::string::npos) return "";
  std::string ext = filename.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

bool attachment_service::is_dangerous(const std::string& filename) const {
  std::string ext = ext_lower(filename);
  for (const auto& d : cfg.attachments.dangerous_extensions)
    if (ext == d) return true;
  return false;
}

bool attachment_service::is_too_large(std::size_t size_bytes, int max_mb) const {
  return max_mb > 0 && size_bytes > static_cast<std::size_t>(max_mb) * 1024 * 1024;
}

void attachment_service::store_attachments(const std::string& email_id, const message& msg) {
  if (!cfg.attachments.enabled) return;
  std::vector<stored_attachment> to_save;
  for (const auto& att : msg.attachments) {
    stored_attachment sa;
    sa.email_id    = email_id;
    sa.mailbox_id  = msg.mailbox_id;
    sa.uid         = msg.uid;
    sa.part_id     = att.part_id;
    sa.filename    = att.filename;
    sa.mime_type   = att.mime_type;
    sa.size_bytes  = att.size_bytes;
    sa.content_id  = att.content_id;
    sa.disposition = att.disposition;
    sa.safe_to_preview = att.safe_to_preview;
    to_save.push_back(std::move(sa));
  }
  if (!to_save.empty()) store.save_email_attachments(email_id, to_save);
}

attachment_fetch_result attachment_service::fetch_attachment(const std::string& attachment_id,
                                                              mail_client& ) {
  attachment_fetch_result result;
  auto rec = store.get_attachment(attachment_id);
  if (!rec) { result.error = "attachment not found: " + attachment_id; return result; }
  if (rec->downloaded && !rec->local_path.empty()) {
    result.ok = true;
    result.local_path = rec->local_path;
    result.sha256 = rec->sha256;
    return result;
  }
  if (is_dangerous(rec->filename)) {
    result.error = "dangerous extension blocked: " + rec->filename;
    return result;
  }
  if (is_too_large(rec->size_bytes, cfg.attachments.max_download_size_mb)) {
    result.error = "attachment too large: " + std::to_string(rec->size_bytes) + " bytes";
    return result;
  }


  result.error = "on-demand fetch not yet implemented (requires IMAP BODY[" + rec->part_id + "] fetch)";
  return result;
}

bool attachment_service::send_to_telegram(const std::string& attachment_id,
                                           const std::string& caption,
                                           std::string& err) {
  auto rec = store.get_attachment(attachment_id);
  if (!rec) { err = "attachment not found"; return false; }
  if (!rec->downloaded || rec->local_path.empty()) { err = "not downloaded"; return false; }
  if (is_dangerous(rec->filename)) { err = "dangerous extension blocked"; return false; }
  if (is_too_large(rec->size_bytes, cfg.attachments.max_telegram_size_mb)) {
    err = "file too large for Telegram (" + std::to_string(cfg.attachments.max_telegram_size_mb) + " MB limit)";
    return false;
  }
  return bot.send_document(rec->local_path, caption, err);
}
